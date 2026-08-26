// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/primitive_service.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

enum class SlotErrorCode : std::uint8_t {
    InvalidArgument,
    Unsupported,
    Ambiguous,
    InvalidDeviceResponse,
    QueryFailed,
};

struct SlotError final {
    SlotErrorCode code{SlotErrorCode::InvalidArgument};
    std::string message;
    std::optional<PrimitiveError> query_error;
};

enum class SlotSelectionKind : std::uint8_t {
    Current,
    Explicit,
    Other,
    All,
};

struct SlotSelection final {
    SlotSelectionKind kind{SlotSelectionKind::Current};
    std::string name;
};

enum class SlotTopologySource : std::uint8_t {
    SlotCount,
    LegacySlotSuffixes,
};

struct SlotTopology final {
    std::vector<std::string> slots;
    SlotTopologySource source{SlotTopologySource::SlotCount};
};

struct PartitionSlotPlan final {
    bool slotted{false};
    std::vector<std::string> partition_names;
    std::vector<std::string> slots;
};

// Parses the Fastboot CLI slot vocabulary without performing device I/O.
// Empty means the device's current slot. Legacy "_a"/"_b" spellings are
// normalized to "a"/"b".
[[nodiscard]] std::expected<SlotSelection, SlotError> parse_slot_selection(
    std::string_view value);

// Internal planner for --slot, set_active, update and flashall. It deliberately
// requires explicit device evidence for every slot decision: unsupported or
// malformed getvars never cause a guessed partition name.
class SlotPlanner final {
public:
    explicit SlotPlanner(PrimitiveService& primitives) noexcept;

    [[nodiscard]] std::expected<SlotTopology, SlotError> query_topology();

    // Resolves one slot suitable for set_active. "all" is invalid here.
    [[nodiscard]] std::expected<std::string, SlotError> resolve_active_slot(
        std::string_view requested_slot);

    // An empty requested_slot selects the current slot for a slotted partition,
    // while a non-slotted partition is returned unchanged. Explicit slot,
    // "other" and "all" requests fail when has-slot:<partition> is false or
    // unsupported.
    [[nodiscard]] std::expected<PartitionSlotPlan, SlotError> plan_partition(
        std::string_view partition,
        std::string_view requested_slot = {});

private:
    [[nodiscard]] std::expected<bool, SlotError> query_has_slot(
        std::string_view partition);
    [[nodiscard]] std::expected<std::string, SlotError> query_current_slot(
        const SlotTopology& topology);
    [[nodiscard]] std::expected<std::vector<std::string>, SlotError>
    resolve_slots(
        const SlotTopology& topology,
        const SlotSelection& selection,
        bool allow_all);

    PrimitiveService& primitives_;
};

}  // namespace kairosboot::fastboot
