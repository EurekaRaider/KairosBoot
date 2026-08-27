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
// policy for individual commands. The caller serializes access and keeps the
// PrimitiveService/session alive until every returned token is destroyed.
class PrimitiveUpdateDevice final : public IUpdateDevice {
public:
    explicit PrimitiveUpdateDevice(
        PrimitiveService& service,
        PrimitiveUpdateDeviceOptions options = {});

    // Enables the bootloader-to-fastbootd transition used by update packages.
    // The coordinator and its discovery/opener/waiter dependencies must outlive
    // this adapter and every prepared token. reconnect_target must describe the
    // already-selected USB device by physical port, fingerprint and product;
    // serial is an additional check and is never used as the primary key.
    PrimitiveUpdateDevice(
        PrimitiveService& service,
        ReconnectCoordinator& reconnect_coordinator,
        ReconnectTarget reconnect_target,
        ReconnectOptions reconnect_options = {},
        PrimitiveUpdateDeviceOptions options = {});

    [[nodiscard]] std::expected<std::string, UpdateDeviceError> getvar(
        std::string_view name,
        const UpdateOperationContext& context) override;

    [[nodiscard]] std::expected<std::unique_ptr<IPreparedDeviceTask>,
                                UpdateDeviceError>
    prepare_task(UpdateDeviceTaskInput input,
                 const UpdateOperationContext& context) override;

private:
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
    std::optional<std::uint64_t> maximum_download_size_;
    std::optional<std::string> current_slot_;
    std::optional<std::vector<std::string>> slot_topology_;
    std::map<std::string, bool, std::less<>> has_slot_;
};

}  // namespace kairosboot::fastboot
