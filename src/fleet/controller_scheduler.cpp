#include "src/fleet/controller_scheduler.hpp"

#include <algorithm>
#include <limits>
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

struct WeightedControllerScheduler::Impl final {
    struct Flow final {
        std::string device_id;
        std::string controller_id;
        std::uint32_t weight{1};
        std::uint64_t remaining{};
        std::uint64_t deficit{};
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

    std::shared_ptr<transport::BufferBudget> budget;
    std::size_t quantum{};
    std::unordered_map<std::string, Flow> flows;
    std::unordered_map<std::string, Controller> controllers;
    std::vector<std::string> controller_order;
    std::size_t controller_cursor{};
    std::unordered_map<std::uint64_t, Outstanding> outstanding;
    std::uint64_t next_token{1};

    [[nodiscard]] std::optional<FleetDispatch> next_for_controller(
        Controller& controller, const std::size_t maximum_bytes) {
        if (controller.flows.empty() || budget->available() == 0) {
            return std::nullopt;
        }

        // Every active flow receives positive credit on its first visit, so a
        // controller with ready work always yields within one round.
        const auto maximum_attempts = controller.flows.size();
        for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
            if (controller.cursor >= controller.flows.size()) {
                controller.cursor = 0;
            }
            auto& flow = flows.at(controller.flows[controller.cursor]);
            if (flow.remaining == 0) {
                controller.cursor = (controller.cursor + 1U) % controller.flows.size();
                continue;
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

            const auto token = next_token++;
            flow.deficit -= dispatch_bytes;
            flow.remaining -= dispatch_bytes;
            outstanding.emplace(token, Outstanding{flow.device_id, dispatch_bytes});

            if (flow.remaining == 0 || flow.deficit == 0) {
                controller.cursor = (controller.cursor + 1U) % controller.flows.size();
            }

            return FleetDispatch{token,
                                 flow.device_id,
                                 flow.controller_id,
                                 dispatch_bytes,
                                 std::move(*memory)};
        }
        return std::nullopt;
    }
};

WeightedControllerScheduler::WeightedControllerScheduler(
    std::shared_ptr<transport::BufferBudget> budget, const std::size_t quantum_bytes)
    : impl_(std::make_unique<Impl>()) {
    impl_->budget = std::move(budget);
    impl_->quantum = quantum_bytes;
}

WeightedControllerScheduler::~WeightedControllerScheduler() = default;

WeightedControllerScheduler::WeightedControllerScheduler(
    WeightedControllerScheduler&&) noexcept = default;

WeightedControllerScheduler& WeightedControllerScheduler::operator=(
    WeightedControllerScheduler&&) noexcept = default;

bool WeightedControllerScheduler::add_flow(DeviceFlowSpec flow) {
    if (impl_ == nullptr || impl_->budget == nullptr || impl_->quantum == 0 ||
        flow.device_id.empty() || flow.controller_id.empty() || flow.weight == 0 ||
        impl_->flows.contains(flow.device_id)) {
        return false;
    }

    const auto device_id = flow.device_id;
    const auto controller_id = flow.controller_id;
    auto controller = impl_->controllers.find(controller_id);
    if (controller == impl_->controllers.end()) {
        impl_->controller_order.push_back(controller_id);
        controller = impl_->controllers
                         .emplace(controller_id,
                                  Impl::Controller{controller_id, {}, 0})
                         .first;
    }
    controller->second.flows.push_back(device_id);
    impl_->flows.emplace(device_id,
                         Impl::Flow{device_id,
                                    controller_id,
                                    flow.weight,
                                    flow.bytes_remaining,
                                    0});
    return true;
}

std::optional<FleetDispatch> WeightedControllerScheduler::next(
    const std::size_t maximum_bytes) {
    if (impl_ == nullptr || impl_->budget == nullptr || maximum_bytes == 0 ||
        impl_->controller_order.empty()) {
        return std::nullopt;
    }

    for (std::size_t attempt = 0; attempt < impl_->controller_order.size(); ++attempt) {
        if (impl_->controller_cursor >= impl_->controller_order.size()) {
            impl_->controller_cursor = 0;
        }
        auto& controller =
            impl_->controllers.at(impl_->controller_order[impl_->controller_cursor]);
        impl_->controller_cursor =
            (impl_->controller_cursor + 1U) % impl_->controller_order.size();
        auto dispatch = impl_->next_for_controller(controller, maximum_bytes);
        if (dispatch.has_value()) {
            return dispatch;
        }
    }
    return std::nullopt;
}

bool WeightedControllerScheduler::finish(FleetDispatch&& dispatch,
                                         const std::size_t bytes_not_sent) noexcept {
    if (impl_ == nullptr || dispatch.token_ == 0 || bytes_not_sent > dispatch.bytes_) {
        return false;
    }
    const auto outstanding = impl_->outstanding.find(dispatch.token_);
    if (outstanding == impl_->outstanding.end() ||
        outstanding->second.device_id != dispatch.device_id_ ||
        outstanding->second.bytes != dispatch.bytes_) {
        return false;
    }

    if (bytes_not_sent != 0) {
        auto flow = impl_->flows.find(dispatch.device_id_);
        if (flow == impl_->flows.end()) {
            return false;
        }
        flow->second.remaining = saturating_add(flow->second.remaining, bytes_not_sent);
    }

    impl_->outstanding.erase(outstanding);
    dispatch.memory_ = {};
    dispatch.token_ = 0;
    dispatch.bytes_ = 0;
    return true;
}

std::uint64_t WeightedControllerScheduler::remaining(
    const std::string_view device_id) const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    for (const auto& [ignored, flow] : impl_->flows) {
        static_cast<void>(ignored);
        if (flow.device_id == device_id) {
            return flow.remaining;
        }
    }
    return 0;
}

std::size_t WeightedControllerScheduler::flow_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->flows.size();
}

std::size_t WeightedControllerScheduler::outstanding_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->outstanding.size();
}

}  // namespace kairosboot::fleet
