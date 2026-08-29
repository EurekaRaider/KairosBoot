// SPDX-License-Identifier: MIT
#include "src/api/operation_state.hpp"

#include <exception>
#include <new>
#include <utility>

namespace kairosboot::api {
namespace {

[[nodiscard]] OperationErrorPayload cancelled_error() {
    return {
        .status = KB_E_CANCELLED,
        .message = "operation cancelled",
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = {},
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    };
}

[[nodiscard]] OperationErrorPayload internal_error(const char* message) {
    return {
        .status = KB_E_INTERNAL,
        .message = message,
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = {},
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    };
}

}  // namespace

OperationOutcome OperationOutcome::succeeded(
    std::shared_ptr<const CommandResultPayload> command_result) {
    return {OperationPhase::Succeeded, std::nullopt, std::move(command_result)};
}

OperationOutcome OperationOutcome::failed(OperationErrorPayload error) {
    if (error.status == KB_OK || error.status == KB_E_CANCELLED) {
        error.status = KB_E_INTERNAL;
    }
    if (error.message.empty()) {
        error.message = "operation failed";
    }
    return {OperationPhase::Failed, std::move(error), {}};
}

OperationOutcome OperationOutcome::cancelled(OperationErrorPayload error) {
    error.status = KB_E_CANCELLED;
    if (error.message.empty()) {
        error.message = "operation cancelled";
    }
    return {OperationPhase::Cancelled, std::move(error), {}};
}

OperationState::OperationState(Task task) : task_(std::move(task)) {}

OperationState::~OperationState() {
    cancel();
    if (!worker_.joinable()) {
        return;
    }
    if (worker_.get_id() == std::this_thread::get_id()) {
        std::terminate();
    }
    worker_.join();
}

bool OperationState::start() {
    std::scoped_lock lock(mutex_);
    if (started_ || phase_ != OperationPhase::Created || cancel_requested_) {
        return false;
    }

    worker_ = std::thread(&OperationState::run, this);
    started_ = true;
    return true;
}

void OperationState::cancel() noexcept {
    CancellationHook hook;
    bool publish_created_cancellation = false;
    {
        std::scoped_lock lock(mutex_);
        if (terminal(phase_) || cancel_requested_) {
            return;
        }

        cancel_requested_ = true;
        if (!started_) {
            phase_ = OperationPhase::Cancelled;
            error_ = cancelled_error();
            publish_created_cancellation = true;
        } else if (cancellation_hook_.has_value()) {
            ++hook_invocations_in_flight_;
            hook = cancellation_hook_->callback;
        }
    }

    static_cast<void>(cancellation_.request_stop());
    if (hook) {
        try {
            hook();
        } catch (...) {
            // Cancellation must remain noexcept. The task observes the stop token
            // and determines its terminal result after transport drain.
        }
        finish_hook_invocation();
    }
    if (publish_created_cancellation) {
        state_changed_.notify_all();
    }
}

bool OperationState::cancel_requested() const noexcept {
    std::scoped_lock lock(mutex_);
    return cancel_requested_;
}

OperationPhase OperationState::phase() const noexcept {
    std::scoped_lock lock(mutex_);
    return phase_;
}

kb_status_t OperationState::status() const noexcept {
    std::scoped_lock lock(mutex_);
    switch (phase_) {
        case OperationPhase::Succeeded:
            return KB_OK;
        case OperationPhase::Cancelled:
            return KB_E_CANCELLED;
        case OperationPhase::Failed:
            return error_.has_value() ? error_->status : KB_E_INTERNAL;
        case OperationPhase::Created:
        case OperationPhase::Running:
            return KB_E_BUSY;
    }
    return KB_E_INTERNAL;
}

std::optional<OperationErrorPayload> OperationState::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

std::shared_ptr<const CommandResultPayload> OperationState::command_result() const {
    std::scoped_lock lock(mutex_);
    return command_result_;
}

OperationWaitResult OperationState::wait_for(
    const std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    if (terminal(phase_)) {
        return OperationWaitResult::Terminal;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return OperationWaitResult::Timeout;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto remaining_room =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::time_point::max() - now);
    const auto deadline = timeout >= remaining_room
                              ? std::chrono::steady_clock::time_point::max()
                              : now + timeout;
    const auto reached_terminal = state_changed_.wait_until(
        lock, deadline, [this] { return terminal(phase_); });
    return reached_terminal ? OperationWaitResult::Terminal
                            : OperationWaitResult::Timeout;
}

void OperationState::wait() const {
    std::unique_lock lock(mutex_);
    state_changed_.wait(lock, [this] { return terminal(phase_); });
}

OperationState::CancellationRegistration::CancellationRegistration(
    OperationState* owner,
    const std::uint64_t generation) noexcept
    : owner_(owner), generation_(generation) {}

OperationState::CancellationRegistration::~CancellationRegistration() {
    reset();
}

OperationState::CancellationRegistration::CancellationRegistration(
    CancellationRegistration&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      generation_(std::exchange(other.generation_, 0)) {}

OperationState::CancellationRegistration&
OperationState::CancellationRegistration::operator=(
    CancellationRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        generation_ = std::exchange(other.generation_, 0);
    }
    return *this;
}

bool OperationState::CancellationRegistration::active() const noexcept {
    return owner_ != nullptr;
}

void OperationState::CancellationRegistration::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->unregister_cancellation_hook(generation_);
        owner_ = nullptr;
        generation_ = 0;
    }
}

OperationState::TaskContext::TaskContext(OperationState& owner) noexcept
    : owner_(owner) {}

std::stop_token OperationState::TaskContext::cancellation_token() const noexcept {
    return owner_.cancellation_.get_token();
}

bool OperationState::TaskContext::cancel_requested() const noexcept {
    return owner_.cancel_requested();
}

OperationState::CancellationRegistration
OperationState::TaskContext::register_cancellation_hook(CancellationHook hook) {
    return owner_.register_cancellation_hook(std::move(hook));
}

bool OperationState::terminal(const OperationPhase phase) noexcept {
    return phase == OperationPhase::Succeeded || phase == OperationPhase::Failed ||
           phase == OperationPhase::Cancelled;
}

OperationState::CancellationRegistration
OperationState::register_cancellation_hook(CancellationHook hook) {
    if (!hook) {
        return {};
    }

    std::uint64_t generation = 0;
    bool invoke_now = false;
    {
        std::scoped_lock lock(mutex_);
        if (terminal(phase_)) {
            return {};
        }
        if (cancel_requested_) {
            invoke_now = true;
        } else if (!cancellation_hook_.has_value()) {
            generation = next_hook_generation_++;
            cancellation_hook_ = HookRecord{generation, std::move(hook)};
        } else {
            return {};
        }
    }

    if (invoke_now) {
        try {
            hook();
        } catch (...) {
            // The stop token remains authoritative when a hook rejects cancel.
        }
        return {};
    }
    return CancellationRegistration(this, generation);
}

void OperationState::unregister_cancellation_hook(
    const std::uint64_t generation) noexcept {
    std::unique_lock lock(mutex_);
    if (cancellation_hook_.has_value() &&
        cancellation_hook_->generation == generation) {
        cancellation_hook_.reset();
    }
    hook_finished_.wait(lock, [this] { return hook_invocations_in_flight_ == 0; });
}

void OperationState::finish_hook_invocation() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (hook_invocations_in_flight_ != 0) {
            --hook_invocations_in_flight_;
        }
    }
    hook_finished_.notify_all();
}

void OperationState::clear_cancellation_hook_before_terminal() noexcept {
    std::unique_lock lock(mutex_);
    cancellation_hook_.reset();
    hook_finished_.wait(lock, [this] { return hook_invocations_in_flight_ == 0; });
}

void OperationState::publish_terminal(OperationOutcome outcome) noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (phase_ != OperationPhase::Running) {
            return;
        }

        if (!terminal(outcome.phase)) {
            outcome = OperationOutcome::failed(
                internal_error("operation task returned a non-terminal state"));
        }
        if (cancel_requested_ && outcome.phase == OperationPhase::Succeeded) {
            outcome = OperationOutcome::cancelled({
                .status = KB_E_CANCELLED,
                .message = "operation cancelled after completing its work",
                .native_code = 0,
                .transfer_state = KB_TRANSFER_FULLY_TRANSFERRED,
                .device_identifier = {},
                .device_message = {},
                .command_messages = {},
                .inbound_expected = std::nullopt,
                .inbound_transferred = 0,
                .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
                .session_poisoned = false,
            });
        }
        if (outcome.phase == OperationPhase::Succeeded) {
            outcome.error.reset();
        } else if (!outcome.error.has_value()) {
            outcome = outcome.phase == OperationPhase::Cancelled
                          ? OperationOutcome::cancelled()
                          : OperationOutcome::failed(
                                internal_error("operation task returned no error"));
        }
        if (outcome.phase != OperationPhase::Succeeded) {
            outcome.command_result.reset();
        }

        phase_ = outcome.phase;
        error_ = std::move(outcome.error);
        command_result_ = std::move(outcome.command_result);
    }
    state_changed_.notify_all();
}

void OperationState::run() noexcept {
    Task task;
    {
        std::scoped_lock lock(mutex_);
        if (phase_ != OperationPhase::Created) {
            return;
        }
        phase_ = OperationPhase::Running;
        task = std::move(task_);
        task_ = {};
    }
    state_changed_.notify_all();

    OperationOutcome outcome;
    if (cancellation_.stop_requested()) {
        outcome = OperationOutcome::cancelled();
    } else if (!task) {
        outcome = OperationOutcome::failed(
            internal_error("operation task is empty"));
    } else {
        TaskContext context(*this);
        try {
            outcome = task(context);
        } catch (const std::bad_alloc&) {
            outcome = OperationOutcome::failed({
                .status = KB_E_OUT_OF_MEMORY,
                .message = "operation task exhausted memory",
                .native_code = 0,
                .transfer_state = KB_TRANSFER_NOT_SENT,
                .device_identifier = {},
                .device_message = {},
                .command_messages = {},
                .inbound_expected = std::nullopt,
                .inbound_transferred = 0,
                .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
                .session_poisoned = false,
            });
        } catch (const std::exception& error) {
            outcome = OperationOutcome::failed({
                .status = KB_E_INTERNAL,
                .message = error.what(),
                .native_code = 0,
                .transfer_state = KB_TRANSFER_NOT_SENT,
                .device_identifier = {},
                .device_message = {},
                .command_messages = {},
                .inbound_expected = std::nullopt,
                .inbound_transferred = 0,
                .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
                .session_poisoned = false,
            });
        } catch (...) {
            outcome = OperationOutcome::failed(
                internal_error("operation task threw an unknown exception"));
        }
    }

    // Release task captures before publishing the terminal state. Callers may
    // start the next operation as soon as wait() observes that state, so
    // per-device leases and other task-scoped resources must already be gone.
    task = {};

    // Deregistration waits for a concurrently executing cancellation hook. The
    // terminal state is deliberately published only after that drain completes.
    clear_cancellation_hook_before_terminal();
    publish_terminal(std::move(outcome));
}

}  // namespace kairosboot::api
