#include "src/fleet/controller_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kairosboot::fleet {

namespace {

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] std::uint64_t saturating_multiply(const std::size_t quantum,
                                                const std::uint32_t weight) noexcept {
    const auto quantum64 = static_cast<std::uint64_t>(quantum);
    if (weight != 0 &&
        quantum64 > std::numeric_limits<std::uint64_t>::max() / weight) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return quantum64 * weight;
}

[[nodiscard]] transport::TransferPermitSettlement permit_settlement(
    const FleetDispatchSettlement result) noexcept {
    switch (result) {
        case FleetDispatchSettlement::fully_transferred:
            return transport::TransferPermitSettlement::fully_transferred;
        case FleetDispatchSettlement::not_submitted:
            return transport::TransferPermitSettlement::not_submitted;
        case FleetDispatchSettlement::partial_or_unknown:
            return transport::TransferPermitSettlement::partial_or_unknown;
    }
    return transport::TransferPermitSettlement::partial_or_unknown;
}

}  // namespace

FleetDispatch::FleetDispatch(const std::uint64_t token,
                             std::string device_id,
                             std::string controller_id,
                             const std::size_t bytes,
                             transport::BufferLease memory)
    : token_(token),
      device_id_(std::move(device_id)),
      controller_id_(std::move(controller_id)),
      bytes_(bytes),
      memory_(std::move(memory)) {}

FleetDispatch::FleetDispatch(FleetDispatch&& other) noexcept
    : token_(std::exchange(other.token_, 0)),
      device_id_(std::move(other.device_id_)),
      controller_id_(std::move(other.controller_id_)),
      bytes_(std::exchange(other.bytes_, 0)),
      memory_(std::move(other.memory_)) {}

std::uint64_t FleetDispatch::token() const noexcept { return token_; }
std::string_view FleetDispatch::device_id() const noexcept { return device_id_; }
std::string_view FleetDispatch::controller_id() const noexcept { return controller_id_; }
std::size_t FleetDispatch::bytes() const noexcept { return bytes_; }
std::span<std::byte> FleetDispatch::buffer() noexcept { return memory_.bytes(); }

struct WeightedControllerScheduler::Impl final
    : std::enable_shared_from_this<WeightedControllerScheduler::Impl>,
      transport::BufferBudgetAvailabilityObserver {
    struct Flow final {
        std::string device_id;
        std::string controller_id;
        std::uint32_t weight{1};
        std::uint64_t remaining{};
        std::uint64_t deficit{};
        std::size_t outstanding_count{};
        std::size_t max_outstanding{1};
        bool requested{};
        bool retired{};
        bool provider_attached{};
    };

    struct Controller final {
        std::string id;
        std::vector<std::string> flows;
        std::size_t cursor{};
    };

    struct Outstanding final {
        std::string device_id;
        std::size_t bytes{};
    };

    struct Grant final {
        std::uint64_t token{};
        std::string device_id;
        std::string controller_id;
        std::size_t bytes{};
        transport::BufferLease memory;
    };

    class PermitProvider final : public transport::TransferPermitProvider {
    public:
        PermitProvider(std::shared_ptr<Impl> impl, std::string device_id)
            : impl_(std::move(impl)), device_id_(std::move(device_id)) {}

        ~PermitProvider() override { cancel_wait(); }

        [[nodiscard]] std::optional<transport::TransferPermit> try_acquire(
            const std::size_t maximum_bytes) override {
            if (cancelled_.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            auto grant = impl_->acquire_for(device_id_, maximum_bytes);
            if (!grant.has_value()) {
                return std::nullopt;
            }
            return make_permit(std::move(grant->memory), grant->token);
        }

        [[nodiscard]] std::uint64_t readiness_generation() const noexcept override {
            return impl_->readiness_generation();
        }

        [[nodiscard]] transport::TransferPermitWaitResult wait_for_ready(
            const std::uint64_t observed_generation,
            const std::chrono::steady_clock::time_point deadline) override {
            return impl_->wait_for_ready(
                device_id_, observed_generation, deadline);
        }

        void cancel_wait() noexcept override {
            if (!cancelled_.exchange(true, std::memory_order_acq_rel)) {
                static_cast<void>(impl_->retire(device_id_));
            }
        }

    private:
        void settle(const std::uint64_t token,
                    const std::size_t bytes,
                    const transport::TransferPermitSettlement result) noexcept override {
            static_cast<void>(impl_->settle(token, {}, bytes, result));
        }

        std::shared_ptr<Impl> impl_;
        std::string device_id_;
        std::atomic<bool> cancelled_{false};
    };

    std::shared_ptr<transport::BufferBudget> budget;
    std::size_t quantum{};
    mutable std::mutex mutex;
    std::mutex notification_mutex;
    std::condition_variable changed;
    std::atomic<std::uint64_t> generation{1};
    std::unordered_map<std::string, Flow> flows;
    std::unordered_map<std::string, Controller> controllers;
    std::vector<std::string> controller_order;
    std::size_t controller_cursor{};
    std::unordered_map<std::uint64_t, Outstanding> outstanding;
    std::uint64_t next_token{1};

    void signal_change() noexcept {
        std::scoped_lock lock(notification_mutex);
        generation.fetch_add(1, std::memory_order_acq_rel);
        changed.notify_all();
    }

    void on_buffer_budget_available() noexcept override { signal_change(); }

    [[nodiscard]] std::uint64_t allocate_token() noexcept {
        std::uint64_t token{};
        do {
            token = next_token++;
        } while (token == 0 || outstanding.contains(token));
        return token;
    }

    [[nodiscard]] std::optional<Grant> next_for_controller(
        Controller& controller,
        const std::size_t maximum_bytes,
        const std::string_view required_device = {}) {
        if (controller.flows.empty() || budget->available() == 0) {
            return std::nullopt;
        }

        const auto maximum_attempts = controller.flows.size();
        for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
            if (controller.cursor >= controller.flows.size()) {
                controller.cursor = 0;
            }
            auto& flow = flows.at(controller.flows[controller.cursor]);
            const bool has_request = !flow.provider_attached || flow.requested;
            if (flow.remaining == 0 || flow.retired || !has_request ||
                flow.outstanding_count >= flow.max_outstanding) {
                controller.cursor = (controller.cursor + 1U) % controller.flows.size();
                continue;
            }

            if (!required_device.empty() && flow.device_id != required_device) {
                return std::nullopt;
            }

            const auto available = budget->available();
            if (available == 0) {
                return std::nullopt;
            }
            const auto bounded_remaining = static_cast<std::size_t>(
                std::min<std::uint64_t>(flow.remaining,
                                        std::numeric_limits<std::size_t>::max()));
            const auto desired = std::min({maximum_bytes, available, bounded_remaining});
            if (desired == 0) {
                return std::nullopt;
            }
            if (flow.deficit == 0) {
                flow.deficit = saturating_add(
                    flow.deficit, saturating_multiply(quantum, flow.weight));
            }
            const auto dispatch_bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(flow.deficit, desired));
            if (dispatch_bytes == 0) {
                controller.cursor = (controller.cursor + 1U) % controller.flows.size();
                continue;
            }

            auto memory = budget->try_acquire(dispatch_bytes);
            if (!memory.has_value()) {
                return std::nullopt;
            }
            const auto token = allocate_token();
            auto grant = Grant{token,
                               flow.device_id,
                               flow.controller_id,
                               dispatch_bytes,
                               std::move(*memory)};
            outstanding.emplace(token, Outstanding{grant.device_id, dispatch_bytes});
            ++flow.outstanding_count;
            if (flow.provider_attached) {
                flow.requested = false;
            }
            flow.deficit -= dispatch_bytes;
            flow.remaining -= dispatch_bytes;
            if (flow.remaining == 0 || flow.deficit == 0) {
                controller.cursor = (controller.cursor + 1U) % controller.flows.size();
            }
            signal_change();
            return grant;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Grant> next(const std::size_t maximum_bytes) {
        std::scoped_lock lock(mutex);
        if (budget == nullptr || maximum_bytes == 0 || controller_order.empty()) {
            return std::nullopt;
        }
        for (std::size_t attempt = 0; attempt < controller_order.size(); ++attempt) {
            if (controller_cursor >= controller_order.size()) {
                controller_cursor = 0;
            }
            auto& controller = controllers.at(controller_order[controller_cursor]);
            controller_cursor = (controller_cursor + 1U) % controller_order.size();
            auto dispatch = next_for_controller(controller, maximum_bytes);
            if (dispatch.has_value()) {
                return dispatch;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Grant> acquire_for(
        const std::string_view device_id, const std::size_t maximum_bytes) {
        std::scoped_lock lock(mutex);
        if (budget == nullptr || maximum_bytes == 0) {
            return std::nullopt;
        }
        Flow* requested = nullptr;
        for (auto& [ignored, flow] : flows) {
            static_cast<void>(ignored);
            if (flow.device_id == device_id) {
                requested = &flow;
                break;
            }
        }
        if (requested == nullptr || !requested->provider_attached ||
            requested->retired || requested->remaining == 0) {
            return std::nullopt;
        }
        requested->requested = true;
        if (requested->outstanding_count >= requested->max_outstanding) {
            return std::nullopt;
        }
        auto controller = controllers.find(requested->controller_id);
        if (controller == controllers.end()) {
            return std::nullopt;
        }
        return next_for_controller(controller->second, maximum_bytes, device_id);
    }

    [[nodiscard]] bool settle(const std::uint64_t token,
                              const std::string_view expected_device,
                              const std::size_t bytes,
                              const transport::TransferPermitSettlement result) noexcept {
        std::scoped_lock lock(mutex);
        const auto entry = outstanding.find(token);
        if (entry == outstanding.end() || entry->second.bytes != bytes ||
            (!expected_device.empty() && entry->second.device_id != expected_device)) {
            return false;
        }
        const auto flow = flows.find(entry->second.device_id);
        if (flow == flows.end() || flow->second.outstanding_count == 0) {
            return false;
        }
        if (result == transport::TransferPermitSettlement::not_submitted) {
            flow->second.remaining = saturating_add(flow->second.remaining, bytes);
        }
        --flow->second.outstanding_count;
        if (result == transport::TransferPermitSettlement::partial_or_unknown) {
            flow->second.retired = true;
            flow->second.requested = false;
        }
        outstanding.erase(entry);
        return true;
    }

    [[nodiscard]] bool retire(const std::string_view device_id) noexcept {
        std::scoped_lock lock(mutex);
        for (auto& [ignored, flow] : flows) {
            static_cast<void>(ignored);
            if (flow.device_id == device_id) {
                flow.retired = true;
                flow.requested = false;
                signal_change();
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t readiness_generation() const noexcept {
        return generation.load(std::memory_order_acquire);
    }

    [[nodiscard]] transport::TransferPermitWaitResult wait_for_ready(
        const std::string_view device_id,
        const std::uint64_t observed_generation,
        const std::chrono::steady_clock::time_point deadline) {
        {
            std::scoped_lock lock(mutex);
            bool found = false;
            for (const auto& [ignored, flow] : flows) {
                static_cast<void>(ignored);
                if (flow.device_id == device_id) {
                    found = true;
                    if (flow.retired) {
                        return transport::TransferPermitWaitResult::cancelled;
                    }
                    break;
                }
            }
            if (!found) {
                return transport::TransferPermitWaitResult::cancelled;
            }
        }

        std::unique_lock notification_lock(notification_mutex);
        const bool notified = changed.wait_until(
            notification_lock, deadline, [this, observed_generation] {
                return generation.load(std::memory_order_acquire) !=
                    observed_generation;
            });
        notification_lock.unlock();
        if (!notified) {
            bool abandoned = false;
            std::scoped_lock lock(mutex);
            for (auto& [ignored, flow] : flows) {
                static_cast<void>(ignored);
                if (flow.device_id == device_id && flow.requested) {
                    flow.requested = false;
                    abandoned = true;
                    break;
                }
            }
            if (abandoned) {
                signal_change();
            }
            return transport::TransferPermitWaitResult::timeout;
        }
        std::scoped_lock lock(mutex);
        for (const auto& [ignored, flow] : flows) {
            static_cast<void>(ignored);
            if (flow.device_id == device_id) {
                return flow.retired
                    ? transport::TransferPermitWaitResult::cancelled
                    : transport::TransferPermitWaitResult::ready;
            }
        }
        return transport::TransferPermitWaitResult::cancelled;
    }
};

WeightedControllerScheduler::WeightedControllerScheduler(
    std::shared_ptr<transport::BufferBudget> budget, const std::size_t quantum_bytes)
    : impl_(std::make_shared<Impl>()) {
    impl_->budget = std::move(budget);
    impl_->quantum = quantum_bytes;
    if (impl_->budget != nullptr) {
        impl_->budget->observe_availability(impl_);
    }
}

WeightedControllerScheduler::~WeightedControllerScheduler() = default;
WeightedControllerScheduler::WeightedControllerScheduler(
    WeightedControllerScheduler&&) noexcept = default;
WeightedControllerScheduler& WeightedControllerScheduler::operator=(
    WeightedControllerScheduler&&) noexcept = default;

bool WeightedControllerScheduler::add_flow(DeviceFlowSpec flow) {
    if (impl_ == nullptr || impl_->budget == nullptr || impl_->quantum == 0 ||
        flow.device_id.empty() || flow.controller_id.empty() || flow.weight == 0 ||
        flow.max_outstanding == 0) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->flows.contains(flow.device_id)) {
        return false;
    }
    const auto device_id = flow.device_id;
    const auto controller_id = flow.controller_id;
    auto controller = impl_->controllers.find(controller_id);
    if (controller == impl_->controllers.end()) {
        impl_->controller_order.push_back(controller_id);
        controller = impl_->controllers
                         .emplace(controller_id, Impl::Controller{controller_id, {}, 0})
                         .first;
    }
    controller->second.flows.push_back(device_id);
    impl_->flows.emplace(
        device_id,
        Impl::Flow{.device_id = device_id,
                   .controller_id = controller_id,
                   .weight = flow.weight,
                   .remaining = flow.bytes_remaining,
                   .max_outstanding = flow.max_outstanding});
    return true;
}

std::optional<FleetDispatch> WeightedControllerScheduler::next(
    const std::size_t maximum_bytes) {
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    auto grant = impl_->next(maximum_bytes);
    if (!grant.has_value()) {
        return std::nullopt;
    }
    return FleetDispatch{grant->token,
                         std::move(grant->device_id),
                         std::move(grant->controller_id),
                         grant->bytes,
                         std::move(grant->memory)};
}

bool WeightedControllerScheduler::finish(FleetDispatch&& dispatch,
                                         const std::size_t bytes_not_sent) noexcept {
    if (dispatch.token_ == 0 || bytes_not_sent > dispatch.bytes_ ||
        (bytes_not_sent != 0 && bytes_not_sent != dispatch.bytes_)) {
        return false;
    }
    const auto result = bytes_not_sent == dispatch.bytes_
        ? FleetDispatchSettlement::not_submitted
        : FleetDispatchSettlement::fully_transferred;
    return finish(std::move(dispatch), result);
}

bool WeightedControllerScheduler::finish(FleetDispatch&& dispatch,
                                         const FleetDispatchSettlement result) noexcept {
    if (impl_ == nullptr || dispatch.token_ == 0) {
        return false;
    }
    if (!impl_->settle(dispatch.token_,
                       dispatch.device_id_,
                       dispatch.bytes_,
                       permit_settlement(result))) {
        return false;
    }
    dispatch.token_ = 0;
    dispatch.bytes_ = 0;
    // Settlement is now committed under the scheduler lock. Releasing the
    // lease returns budget capacity and only then publishes availability.
    dispatch.memory_ = {};
    return true;
}

std::shared_ptr<transport::TransferPermitProvider>
WeightedControllerScheduler::make_permit_provider(
    const std::string_view device_id, const std::size_t max_outstanding) {
    if (impl_ == nullptr) {
        return nullptr;
    }
    std::scoped_lock lock(impl_->mutex);
    for (auto& [ignored, flow] : impl_->flows) {
        static_cast<void>(ignored);
        if (flow.device_id != device_id) {
            continue;
        }
        if (flow.provider_attached || flow.outstanding_count != 0 || flow.retired) {
            return nullptr;
        }
        auto provider = std::make_shared<Impl::PermitProvider>(impl_, flow.device_id);
        flow.provider_attached = true;
        if (max_outstanding != 0) {
            flow.max_outstanding = max_outstanding;
        }
        return provider;
    }
    return nullptr;
}

bool WeightedControllerScheduler::retire_flow(
    const std::string_view device_id) noexcept {
    return impl_ != nullptr && impl_->retire(device_id);
}

bool WeightedControllerScheduler::cancel_flow(
    const std::string_view device_id) noexcept {
    return retire_flow(device_id);
}

std::uint64_t WeightedControllerScheduler::remaining(
    const std::string_view device_id) const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::scoped_lock lock(impl_->mutex);
    for (const auto& [ignored, flow] : impl_->flows) {
        static_cast<void>(ignored);
        if (flow.device_id == device_id) {
            return flow.remaining;
        }
    }
    return 0;
}

std::size_t WeightedControllerScheduler::flow_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->flows.size();
}

std::size_t WeightedControllerScheduler::outstanding_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->outstanding.size();
}

}  // namespace kairosboot::fleet
