// SPDX-License-Identifier: MIT
#pragma once

#include <kairosboot/kairosboot.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace kairosboot::api {

enum class OperationPhase : std::uint8_t {
    Created,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct OperationErrorPayload final {
    kb_status_t status{KB_E_INTERNAL};
    std::string message;
    std::int32_t native_code{0};
    kb_transfer_state_t transfer_state{KB_TRANSFER_NOT_SENT};
    std::string device_identifier;

    [[nodiscard]] bool operator==(const OperationErrorPayload&) const = default;
};

struct OperationOutcome final {
    OperationPhase phase{OperationPhase::Failed};
    std::optional<OperationErrorPayload> error;

    [[nodiscard]] static OperationOutcome succeeded();
    [[nodiscard]] static OperationOutcome failed(OperationErrorPayload error);
    [[nodiscard]] static OperationOutcome cancelled(
        OperationErrorPayload error = {
            KB_E_CANCELLED,
            "operation cancelled",
            0,
            KB_TRANSFER_NOT_SENT,
            {},
        });
};

enum class OperationWaitResult : std::uint8_t {
    Terminal,
    Timeout,
};

// Internal asynchronous operation primitive. One owning handle controls one
// worker thread. Destruction requests cancellation and joins the worker, so task
// and cancellation callbacks cannot outlive the owning handle.
class OperationState final {
public:
    using CancellationHook = std::function<void()>;

    class CancellationRegistration;
    class TaskContext;
    using Task = std::function<OperationOutcome(TaskContext&)>;

    explicit OperationState(Task task);
    ~OperationState();

    OperationState(const OperationState&) = delete;
    OperationState& operator=(const OperationState&) = delete;
    OperationState(OperationState&&) = delete;
    OperationState& operator=(OperationState&&) = delete;

    // Starts the task exactly once. Returns false after cancellation or after a
    // previous start. Thread construction failures propagate to the caller while
    // the operation remains in Created state.
    [[nodiscard]] bool start();

    // Idempotently requests cancellation. An active cancellation hook is invoked
    // at most once and never while the operation mutex is held.
    void cancel() noexcept;

    [[nodiscard]] bool cancel_requested() const noexcept;
    [[nodiscard]] OperationPhase phase() const noexcept;
    [[nodiscard]] kb_status_t status() const noexcept;
    [[nodiscard]] std::optional<OperationErrorPayload> error() const;

    [[nodiscard]] OperationWaitResult wait_for(
        std::chrono::milliseconds timeout) const;
    void wait() const;

    class CancellationRegistration final {
    public:
        CancellationRegistration() noexcept = default;
        ~CancellationRegistration();

        CancellationRegistration(const CancellationRegistration&) = delete;
        CancellationRegistration& operator=(const CancellationRegistration&) = delete;

        CancellationRegistration(CancellationRegistration&& other) noexcept;
        CancellationRegistration& operator=(CancellationRegistration&& other) noexcept;

        [[nodiscard]] bool active() const noexcept;
        void reset() noexcept;

    private:
        friend class OperationState;

        CancellationRegistration(OperationState* owner, std::uint64_t generation) noexcept;

        OperationState* owner_{nullptr};
        std::uint64_t generation_{0};
    };

    class TaskContext final {
    public:
        [[nodiscard]] std::stop_token cancellation_token() const noexcept;
        [[nodiscard]] bool cancel_requested() const noexcept;
        [[nodiscard]] CancellationRegistration register_cancellation_hook(
            CancellationHook hook);

    private:
        friend class OperationState;
        explicit TaskContext(OperationState& owner) noexcept;

        OperationState& owner_;
    };

private:
    struct HookRecord final {
        std::uint64_t generation{0};
        CancellationHook callback;
    };

    [[nodiscard]] static bool terminal(OperationPhase phase) noexcept;
    [[nodiscard]] CancellationRegistration register_cancellation_hook(
        CancellationHook hook);
    void unregister_cancellation_hook(std::uint64_t generation) noexcept;
    void finish_hook_invocation() noexcept;
    void clear_cancellation_hook_before_terminal() noexcept;
    void publish_terminal(OperationOutcome outcome) noexcept;
    void run() noexcept;

    mutable std::mutex mutex_;
    mutable std::condition_variable state_changed_;
    mutable std::condition_variable hook_finished_;
    OperationPhase phase_{OperationPhase::Created};
    std::optional<OperationErrorPayload> error_;
    Task task_;
    std::stop_source cancellation_;
    std::optional<HookRecord> cancellation_hook_;
    std::uint64_t next_hook_generation_{1};
    std::size_t hook_invocations_in_flight_{0};
    bool started_{false};
    bool cancel_requested_{false};
    std::thread worker_;
};

}  // namespace kairosboot::api
