// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/reconnect_coordinator.hpp"
#include "src/fastboot/update_executor.hpp"
#include "src/image/sparse_flash_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

class PrimitiveUpdateSessionActor;

// Sealed capability produced by consuming one exclusive open result. The
// FastbootSession and its verified physical/protocol identity travel in the
// same object, so an unrelated PrimitiveService cannot be paired with a target
// at the actor call site. Production callers may create this only from the
// result of the factory that performed the initial enumerate/open/probe.
class VerifiedInitialSessionBinding final {
public:
    VerifiedInitialSessionBinding(const VerifiedInitialSessionBinding&) = delete;
    VerifiedInitialSessionBinding& operator=(
        const VerifiedInitialSessionBinding&) = delete;
    VerifiedInitialSessionBinding(
        VerifiedInitialSessionBinding&&) noexcept = default;
    VerifiedInitialSessionBinding& operator=(
        VerifiedInitialSessionBinding&&) noexcept = default;

private:
    VerifiedInitialSessionBinding(
        std::unique_ptr<protocol::FastbootSession> session,
        std::unique_ptr<PrimitiveService> service,
        ReconnectTarget reconnect_target) noexcept;

    std::unique_ptr<protocol::FastbootSession> session_;
    std::unique_ptr<PrimitiveService> service_;
    ReconnectTarget reconnect_target_{};

    friend std::expected<VerifiedInitialSessionBinding, UpdateDeviceError>
    bind_initial_reconnect_session(
        OpenedReconnectSession,
        ReconnectTarget);
    friend class PrimitiveUpdateSessionActor;
    friend class PrimitiveUpdateDevice;
};

// Side-effect-free adoption of the initial factory open result. Both an absent
// serial and a present serial are compared exactly; physical port is always the
// primary binding key. No production USB factory currently exposes this proof,
// so production code must use the raw fastbootd-only adapter until it does.
[[nodiscard]] std::expected<VerifiedInitialSessionBinding, UpdateDeviceError>
bind_initial_reconnect_session(
    OpenedReconnectSession opened,
    ReconnectTarget reconnect_target);

struct PrimitiveUpdateProgress final {
    std::size_t part_index{};
    std::size_t part_count{};
    std::uint64_t part_completed_bytes{};
    std::uint64_t part_total_bytes{};
    std::uint64_t completed_bytes{};
    std::uint64_t total_bytes{};
};

enum class PrimitiveUpdateProgressAction : std::uint8_t {
    Continue,
    Cancel,
};

using PrimitiveUpdateProgressObserver =
    std::function<PrimitiveUpdateProgressAction(const PrimitiveUpdateProgress&)>;

struct PrimitiveUpdateDeviceOptions final {
    std::uint64_t host_resparse_limit{image::kDefaultResparseLimitBytes};
    PrimitiveUpdateProgressObserver progress{};
};

// Copies every transport/protocol diagnostic carried by PrimitiveError into
// the update actor contract. The absolute operation context may only refine the
// coarse outcome to Cancelled/TimedOut; it never discards wire diagnostics.
[[nodiscard]] UpdateDeviceError map_primitive_update_error(
    PrimitiveError error,
    const UpdateOperationContext& context = {});

// Adapts one already-selected, serialized PrimitiveService session to the
// update executor's two-phase contract. prepare_task performs sparse planning
// and transfer-source binding before any task executes. Queries whose answer is
// valid only in fastbootd are deferred until the token has obtained the
// verified current session; returned tokens still never reopen artifacts or
// reparse sparse images. Every token resolves the current service through one
// shared actor, so a verified reconnect safely replaces the session for every
// later prepared token.
//
// PrimitiveService is synchronous and retains its session-configured per-I/O
// timeout. This adapter checks the absolute deadline before and after each
// synchronous call; it deliberately does not reset or replace protocol timeout
// policy for individual commands. The caller serializes access. The raw
// adapter borrows its PrimitiveService/session; the reconnect adapter consumes
// and owns its factory-bound initial session until every token is destroyed.
class PrimitiveUpdateDevice final : public IUpdateDevice {
public:
    explicit PrimitiveUpdateDevice(
        PrimitiveService& service,
        PrimitiveUpdateDeviceOptions options = {});

    // Enables the bootloader-to-fastbootd transition used by update packages.
    // The coordinator and its discovery/opener/waiter dependencies must outlive
    // this adapter and every prepared token. initial_binding proves that the
    // initial service was factory-bound to the selected physical USB identity.
    [[nodiscard]] static std::expected<
        std::unique_ptr<PrimitiveUpdateDevice>, UpdateDeviceError>
    create_with_reconnect(
        VerifiedInitialSessionBinding initial_binding,
        ReconnectCoordinator& reconnect_coordinator,
        ReconnectOptions reconnect_options = {},
        PrimitiveUpdateDeviceOptions options = {});

    [[nodiscard]] std::expected<std::string, UpdateDeviceError> getvar(
        std::string_view name,
        const UpdateOperationContext& context) override;

    [[nodiscard]] std::expected<std::unique_ptr<IPreparedDeviceTask>,
                                UpdateDeviceError>
    prepare_task(UpdateDeviceTaskInput input,
                 const UpdateOperationContext& context) override;

    // Internal fleet construction hooks. The provider is late-bound only
    // after all immutable tasks have reported their exact DATA byte totals.
    [[nodiscard]] std::expected<void, UpdateDeviceError>
    configure_transfer_permits(
        std::shared_ptr<transport::TransferPermitProvider> provider,
        const transport::TransferRingConfig& config);
    [[nodiscard]] PrimitiveService& current_service_for_fleet_actor() noexcept;

private:
    struct ReconnectConstructionTag final {};

    PrimitiveUpdateDevice(
        ReconnectConstructionTag,
        VerifiedInitialSessionBinding initial_binding,
        ReconnectCoordinator& reconnect_coordinator,
        ReconnectOptions reconnect_options,
        PrimitiveUpdateDeviceOptions options);

    void synchronize_session_cache_generation() noexcept;
    [[nodiscard]] std::expected<std::uint64_t, UpdateDeviceError>
    maximum_download_size(const UpdateOperationContext& context);
    [[nodiscard]] std::expected<std::string, UpdateDeviceError>
    resolve_partition(const PlannedUpdateTask& task,
                      const UpdateOperationContext& context);
    [[nodiscard]] std::expected<std::string, UpdateDeviceError>
    current_slot(const std::vector<std::string>& topology,
                 const UpdateOperationContext& context);
    [[nodiscard]] std::expected<std::vector<std::string>, UpdateDeviceError>
    slot_topology(const UpdateOperationContext& context);
    [[nodiscard]] std::expected<bool, UpdateDeviceError>
    partition_has_slot(std::string_view partition,
                       const UpdateOperationContext& context);

    std::shared_ptr<PrimitiveUpdateSessionActor> session_actor_;
    PrimitiveUpdateDeviceOptions options_;
    std::size_t cache_generation_{};
    std::optional<std::uint64_t> maximum_download_size_;
    std::optional<std::string> current_slot_;
    std::optional<std::vector<std::string>> slot_topology_;
    std::map<std::string, bool, std::less<>> has_slot_;
};

}  // namespace kairosboot::fastboot
