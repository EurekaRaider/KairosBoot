// SPDX-License-Identifier: MIT
#include "src/transport/windows_usb_topology.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "src/transport/libusb_runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <windows.h>

#include <initguid.h>
#include <cfg.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <setupapi.h>
#include <usbiodef.h>

#if defined(_MSC_VER)
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "SetupAPI.lib")
#endif
#endif

namespace kairosboot::transport {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 4U * 1024U;
constexpr std::size_t kMaximumNativeParentDepth = 64U;
#if defined(_WIN32)
constexpr std::size_t kMaximumPropertyBytes = 64U * 1024U;
#endif

[[nodiscard]] WindowsUsbTopologyError make_error(
    const WindowsUsbTopologyErrorKind kind,
    const WindowsUsbTopologyStage stage,
    std::string message,
    std::string device_instance_id_utf8 = {},
    const WindowsUsbNativeErrorDomain native_domain =
        WindowsUsbNativeErrorDomain::None,
    const std::uint32_t native_code = 0U,
    const unsigned long libusb_session_data = 0UL) {
    return WindowsUsbTopologyError{
        .kind = kind,
        .stage = stage,
        .native_domain = native_domain,
        .native_code = native_code,
        .libusb_session_data = libusb_session_data,
        .device_instance_id_utf8 = std::move(device_instance_id_utf8),
        .message = std::move(message),
    };
}

[[nodiscard]] std::chrono::steady_clock::time_point system_now(
    void*) noexcept {
    return std::chrono::steady_clock::now();
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t continuation = 1U;
             continuation <= continuation_count;
             ++continuation) {
            const auto byte =
                static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }
        if ((continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U) ||
            code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] bool valid_identity_text(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdentityBytes ||
        !valid_utf8(value)) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte != 0x7FU;
    });
}

[[nodiscard]] char ascii_upper(const char character) noexcept {
    return character >= 'a' && character <= 'z'
        ? static_cast<char>(character - ('a' - 'A'))
        : character;
}

[[nodiscard]] bool ascii_iequals(const std::string_view left,
                                 const std::string_view right) noexcept {
    return left.size() == right.size() &&
        std::ranges::equal(left, right, [](const char lhs, const char rhs) {
            return ascii_upper(lhs) == ascii_upper(rhs);
        });
}

[[nodiscard]] bool ascii_istarts_with(const std::string_view value,
                                      const std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        ascii_iequals(value.substr(0U, prefix.size()), prefix);
}

[[nodiscard]] bool ascii_icontains(const std::string_view value,
                                   const std::string_view token) noexcept {
    if (token.empty() || token.size() > value.size()) {
        return false;
    }
    for (std::size_t offset = 0U; offset + token.size() <= value.size();
         ++offset) {
        if (ascii_iequals(value.substr(offset, token.size()), token)) {
            return true;
        }
    }
    return false;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_decimal(
    const std::string_view text,
    const Integer maximum = std::numeric_limits<Integer>::max()) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return std::nullopt;
    }
    unsigned long long parsed = 0U;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed > static_cast<unsigned long long>(maximum)) {
        return std::nullopt;
    }
    return static_cast<Integer>(parsed);
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_exact_hex_token(
    const std::string_view token,
    const std::string_view marker,
    const std::size_t digits) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (token.size() != marker.size() + digits ||
        !ascii_istarts_with(token, marker)) {
        return std::nullopt;
    }
    unsigned int parsed = 0U;
    const auto digits_token = token.substr(marker.size());
    const auto [end, error] = std::from_chars(
        digits_token.data(),
        digits_token.data() + digits_token.size(),
        parsed,
        16);
    if (error != std::errc{} ||
        end != digits_token.data() + digits_token.size() ||
        parsed > static_cast<unsigned int>(
                     std::numeric_limits<Integer>::max())) {
        return std::nullopt;
    }
    return static_cast<Integer>(parsed);
}

[[nodiscard]] bool query_is_valid(
    const WindowsUsbTopologyQuery& query) noexcept {
    return query.libusb_session_data != 0UL &&
        valid_identity_text(query.device_instance_id_utf8) &&
        query.vendor_id != 0U && query.product_id != 0U &&
        query.bus_number != 0U &&
        query.device_address != 0U && !query.port_numbers.empty() &&
        query.port_numbers.size() <= kMaximumWindowsUsbTopologyDepth &&
        std::ranges::none_of(query.port_numbers, [](const std::uint8_t port) {
            return port == 0U;
        }) &&
        (!query.serial_utf8.has_value() ||
         valid_identity_text(*query.serial_utf8));
}

[[nodiscard]] std::optional<WindowsUsbTopologyError> interrupted(
    const WindowsUsbTopologyStage stage,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token,
    const WindowsUsbTopologyNow now,
    void* const now_context,
    const unsigned long libusb_session_data = 0UL) {
    if (stop_token.stop_requested()) {
        return make_error(WindowsUsbTopologyErrorKind::Cancelled,
                          stage,
                          "Windows USB topology discovery was cancelled",
                          {},
                          WindowsUsbNativeErrorDomain::None,
                          0U,
                          libusb_session_data);
    }
    if (deadline != std::chrono::steady_clock::time_point::max() &&
        now(now_context) >= deadline) {
        return make_error(WindowsUsbTopologyErrorKind::TimedOut,
                          stage,
                          "Windows USB topology discovery deadline expired",
                          {},
                          WindowsUsbNativeErrorDomain::None,
                          0U,
                          libusb_session_data);
    }
    return std::nullopt;
}

[[nodiscard]] bool contains_case_insensitive(
    const std::vector<std::string>& values,
    const std::string_view expected) noexcept {
    return std::ranges::any_of(values, [expected](const std::string& value) {
        return ascii_iequals(value, expected);
    });
}

[[nodiscard]] bool hub_chain_is_unique(
    const WindowsUsbTopologyNode& node) noexcept {
    for (std::size_t index = 0U;
         index < node.hub_instance_ids_utf8.size();
         ++index) {
        if (ascii_iequals(node.hub_instance_ids_utf8[index],
                          node.root_controller_instance_id_utf8) ||
            ascii_iequals(node.hub_instance_ids_utf8[index],
                          node.device_instance_id_utf8)) {
            return false;
        }
        for (std::size_t other = index + 1U;
             other < node.hub_instance_ids_utf8.size();
             ++other) {
            if (ascii_iequals(node.hub_instance_ids_utf8[index],
                              node.hub_instance_ids_utf8[other])) {
                return false;
            }
        }
    }
    return !ascii_iequals(node.root_controller_instance_id_utf8,
                          node.device_instance_id_utf8);
}

[[nodiscard]] bool node_shape_is_valid(
    const WindowsUsbTopologyNode& node) {
    if (node.libusb_session_data == 0UL ||
        !valid_identity_text(node.device_instance_id_utf8) ||
        !valid_identity_text(node.root_controller_instance_id_utf8) ||
        !valid_identity_text(node.location_path_utf8) ||
        node.vendor_id == 0U || node.product_id == 0U ||
        node.bus_number == 0U || node.device_address == 0U ||
        node.port_numbers.empty() ||
        node.port_numbers.size() > kMaximumWindowsUsbTopologyDepth ||
        node.hub_instance_ids_utf8.empty() ||
        node.hub_instance_ids_utf8.size() != node.port_numbers.size() ||
        node.validation_chain_system_nodes.size() !=
            node.validation_chain_instance_ids_utf8.size() ||
        node.validation_chain_instance_ids_utf8.size() < 3U ||
        node.validation_chain_instance_ids_utf8.size() >
            kMaximumNativeParentDepth ||
        std::ranges::any_of(node.port_numbers, [](const std::uint8_t port) {
            return port == 0U;
        }) ||
        (node.serial_utf8.has_value() &&
         !valid_identity_text(*node.serial_utf8)) ||
        std::ranges::any_of(node.hub_instance_ids_utf8,
                            [](const std::string& identity) {
                                return !valid_identity_text(identity);
                            }) ||
        std::ranges::any_of(node.validation_chain_instance_ids_utf8,
                            [](const std::string& identity) {
                                return !valid_identity_text(identity);
                            }) ||
        !hub_chain_is_unique(node) ||
        !ascii_iequals(node.validation_chain_instance_ids_utf8.front(),
                       node.device_instance_id_utf8) ||
        !ascii_iequals(node.validation_chain_instance_ids_utf8.back(),
                       node.root_controller_instance_id_utf8)) {
        return false;
    }
    for (std::size_t index = 0U;
         index < node.validation_chain_instance_ids_utf8.size();
         ++index) {
        if (node.validation_chain_system_nodes[index] == 0UL) {
            return false;
        }
        for (std::size_t other = index + 1U;
             other < node.validation_chain_instance_ids_utf8.size();
             ++other) {
            if (node.validation_chain_system_nodes[index] ==
                    node.validation_chain_system_nodes[other] ||
                ascii_iequals(node.validation_chain_instance_ids_utf8[index],
                              node.validation_chain_instance_ids_utf8[other])) {
                return false;
            }
        }
    }
    if (std::ranges::any_of(node.hub_instance_ids_utf8,
                            [&node](const std::string& hub) {
                                return !contains_case_insensitive(
                                    node.validation_chain_instance_ids_utf8,
                                    hub);
                            })) {
        return false;
    }
    const auto location =
        parse_windows_usb_location_path(node.location_path_utf8);
    return location.has_value() &&
        location->root_hub_index == node.root_hub_index &&
        location->port_numbers == node.port_numbers &&
        !location->interface_number.has_value();
}

[[nodiscard]] bool node_less(const WindowsUsbTopologyNode& left,
                             const WindowsUsbTopologyNode& right) {
    return std::tie(left.libusb_session_data,
                    left.device_instance_id_utf8,
                    left.root_controller_instance_id_utf8,
                    left.hub_instance_ids_utf8,
                    left.location_path_utf8,
                    left.root_hub_index,
                    left.vendor_id,
                    left.product_id,
                    left.bus_number,
                    left.device_address,
                    left.port_numbers,
                    left.serial_utf8,
                    left.interface_fingerprint.interface_number,
                    left.interface_fingerprint.alternate_setting,
                    left.interface_fingerprint.interface_class,
                    left.interface_fingerprint.interface_subclass,
                    left.interface_fingerprint.interface_protocol,
                    left.validation_chain_system_nodes,
                    left.validation_chain_instance_ids_utf8) <
        std::tie(right.libusb_session_data,
                 right.device_instance_id_utf8,
                 right.root_controller_instance_id_utf8,
                 right.hub_instance_ids_utf8,
                 right.location_path_utf8,
                 right.root_hub_index,
                 right.vendor_id,
                 right.product_id,
                 right.bus_number,
                 right.device_address,
                 right.port_numbers,
                 right.serial_utf8,
                 right.interface_fingerprint.interface_number,
                 right.interface_fingerprint.alternate_setting,
                 right.interface_fingerprint.interface_class,
                 right.interface_fingerprint.interface_subclass,
                 right.interface_fingerprint.interface_protocol,
                 right.validation_chain_system_nodes,
                 right.validation_chain_instance_ids_utf8);
}

struct HardwareIdentity final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::optional<std::uint8_t> interface_number;
};

[[nodiscard]] std::optional<HardwareIdentity> parse_hardware_identity(
    const std::string_view value) noexcept {
    // PnP hardware IDs have the shape ENUMERATOR\\device-id[\\instance-id].
    // Only exact ampersand-delimited tokens in device-id are authoritative.
    // Substring scanning would accept attacker-controlled or malformed values
    // such as XVID_18D1, VID_18D10, or a VID_ token in the serial segment.
    const auto first_separator = value.find('\\');
    if (first_separator == std::string_view::npos ||
        !ascii_iequals(value.substr(0U, first_separator), "USB")) {
        return std::nullopt;
    }
    const auto device_id_begin = first_separator + 1U;
    const auto second_separator = value.find('\\', device_id_begin);
    const auto device_id = value.substr(
        device_id_begin,
        second_separator == std::string_view::npos
            ? std::string_view::npos
            : second_separator - device_id_begin);
    if (device_id.empty()) {
        return std::nullopt;
    }

    std::optional<std::uint16_t> vendor;
    std::optional<std::uint16_t> product;
    std::optional<std::uint8_t> interface_number;
    std::size_t begin = 0U;
    while (begin <= device_id.size()) {
        const auto end = device_id.find('&', begin);
        const auto token = device_id.substr(
            begin,
            end == std::string_view::npos ? std::string_view::npos
                                          : end - begin);
        if (token.empty()) {
            return std::nullopt;
        }
        if (ascii_istarts_with(token, "VID_")) {
            if (vendor.has_value()) {
                return std::nullopt;
            }
            vendor = parse_exact_hex_token<std::uint16_t>(token, "VID_", 4U);
            if (!vendor.has_value()) {
                return std::nullopt;
            }
        } else if (ascii_istarts_with(token, "PID_")) {
            if (product.has_value()) {
                return std::nullopt;
            }
            product =
                parse_exact_hex_token<std::uint16_t>(token, "PID_", 4U);
            if (!product.has_value()) {
                return std::nullopt;
            }
        } else if (ascii_istarts_with(token, "MI_")) {
            if (interface_number.has_value()) {
                return std::nullopt;
            }
            interface_number =
                parse_exact_hex_token<std::uint8_t>(token, "MI_", 2U);
            if (!interface_number.has_value()) {
                return std::nullopt;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    if (!vendor.has_value() || !product.has_value()) {
        return std::nullopt;
    }
    return HardwareIdentity{
        .vendor_id = *vendor,
        .product_id = *product,
        .interface_number = interface_number,
    };
}

[[nodiscard]] std::string format_location_path(
    const WindowsUsbLocationPath& location) {
    std::string result = location.controller_prefix_utf8;
    result += "#USBROOT(" + std::to_string(location.root_hub_index) + ")";
    for (const auto port : location.port_numbers) {
        result += "#USB(" + std::to_string(port) + ")";
    }
    return result;
}

[[nodiscard]] std::expected<WindowsUsbTopologyNode, WindowsUsbTopologyError>
map_native_snapshot(
    const WindowsUsbTopologyQuery& query,
    const WindowsUsbNativeSnapshot& snapshot,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) {
    if (snapshot.requested_system_node != query.libusb_session_data) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::IdentityMismatch,
            WindowsUsbTopologyStage::Correlation,
            "native snapshot does not belong to the requested libusb DEVINST",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }
    if (snapshot.chain_leaf_to_root.empty()) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::IdentityChanged,
            WindowsUsbTopologyStage::Correlation,
            "the saved libusb DEVINST generation is no longer present",
            query.device_instance_id_utf8,
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }
    if (snapshot.chain_leaf_to_root.size() > kMaximumNativeParentDepth) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::TopologyTooDeep,
            WindowsUsbTopologyStage::ParentTraversal,
            "Windows devnode ancestry exceeds the supported depth",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }

    std::vector<unsigned long> visited_nodes;
    std::vector<std::string> visited_ids;
    std::vector<std::string> hubs_leaf_to_root;
    std::optional<std::size_t> first_hub_index;
    for (std::size_t index = 0U;
         index < snapshot.chain_leaf_to_root.size();
         ++index) {
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::Correlation,
                deadline,
                stop_token,
                system_now,
                nullptr,
                query.libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        const auto& raw = snapshot.chain_leaf_to_root[index];
        if (raw.system_node == 0UL || !raw.present ||
            !valid_identity_text(raw.device_instance_id_utf8) ||
            (raw.exposes_usb_hub_interface &&
             raw.exposes_usb_host_controller_interface) ||
            std::ranges::any_of(raw.hardware_ids_utf8,
                                [](const std::string& value) {
                                    return !valid_identity_text(value);
                                }) ||
            std::ranges::any_of(raw.location_paths_utf8,
                                [](const std::string& value) {
                                    return !valid_identity_text(value);
                                })) {
            return std::unexpected(make_error(
                raw.present ? WindowsUsbTopologyErrorKind::MalformedSnapshot
                            : WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::Correlation,
                raw.present
                    ? "native DEVINST snapshot contains malformed identity data"
                    : "a DEVINST in the parent chain is no longer present",
                raw.device_instance_id_utf8,
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        if (index == 0U && raw.system_node != query.libusb_session_data) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::Correlation,
                "the native parent chain no longer starts at the libusb DEVINST",
                raw.device_instance_id_utf8,
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        if (index == 0U &&
            !ascii_iequals(raw.device_instance_id_utf8,
                           query.device_instance_id_utf8)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::Correlation,
                "the libusb DEVINST was reused by another PnP device generation",
                raw.device_instance_id_utf8,
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        if (std::ranges::find(visited_nodes, raw.system_node) !=
                visited_nodes.end() ||
            contains_case_insensitive(visited_ids,
                                      raw.device_instance_id_utf8)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::AmbiguousMapping,
                WindowsUsbTopologyStage::ParentTraversal,
                "native DEVINST parent chain contains a duplicate or cycle",
                raw.device_instance_id_utf8,
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        visited_nodes.push_back(raw.system_node);
        visited_ids.push_back(raw.device_instance_id_utf8);

        const bool last = index + 1U == snapshot.chain_leaf_to_root.size();
        if (last != raw.exposes_usb_host_controller_interface) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                WindowsUsbTopologyStage::ParentTraversal,
                "native chain must terminate at exactly one USB host controller",
                raw.device_instance_id_utf8,
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        if (last) {
            if (raw.parent_system_node.has_value()) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::ParentTraversal,
                    "truncated host-controller node unexpectedly has a parent",
                    raw.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
        } else if (!raw.parent_system_node.has_value() ||
                   *raw.parent_system_node !=
                       snapshot.chain_leaf_to_root[index + 1U].system_node) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::ParentTraversal,
                "native DEVINST parent relation changed during the snapshot",
                raw.device_instance_id_utf8,
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        if (raw.exposes_usb_hub_interface) {
            if (!first_hub_index.has_value()) {
                first_hub_index = index;
            }
            hubs_leaf_to_root.push_back(raw.device_instance_id_utf8);
        }
    }

    if (!first_hub_index.has_value() || hubs_leaf_to_root.size() !=
            query.port_numbers.size()) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::IdentityMismatch,
            WindowsUsbTopologyStage::ParentTraversal,
            "USB hub ancestry does not match the libusb physical port depth",
            snapshot.chain_leaf_to_root.front().device_instance_id_utf8,
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }

    bool found_hardware_identity = false;
    std::optional<WindowsUsbLocationPath> selected_location;
    for (std::size_t index = 0U; index < *first_hub_index; ++index) {
        const auto& raw = snapshot.chain_leaf_to_root[index];
        std::vector<std::string_view> identity_values;
        identity_values.reserve(raw.hardware_ids_utf8.size() + 1U);
        identity_values.push_back(raw.device_instance_id_utf8);
        for (const auto& hardware_id : raw.hardware_ids_utf8) {
            identity_values.push_back(hardware_id);
        }
        for (const auto identity_value : identity_values) {
            const auto hardware = parse_hardware_identity(identity_value);
            if (!hardware.has_value()) {
                continue;
            }
            found_hardware_identity = true;
            if (hardware->vendor_id != query.vendor_id ||
                hardware->product_id != query.product_id ||
                (hardware->interface_number.has_value() &&
                 *hardware->interface_number !=
                     query.interface_fingerprint.interface_number)) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::IdentityMismatch,
                    WindowsUsbTopologyStage::Correlation,
                    "the exact DEVINST hardware identity differs from libusb",
                    raw.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
        }

        for (const auto& path : raw.location_paths_utf8) {
            if (!ascii_icontains(path, "USBROOT(")) {
                continue;
            }
            auto parsed = parse_windows_usb_location_path(path);
            if (!parsed.has_value()) {
                auto error = parsed.error();
                error.device_instance_id_utf8 = raw.device_instance_id_utf8;
                error.libusb_session_data = query.libusb_session_data;
                return std::unexpected(std::move(error));
            }
            if (parsed->port_numbers != query.port_numbers ||
                (parsed->interface_number.has_value() &&
                 *parsed->interface_number !=
                     query.interface_fingerprint.interface_number)) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::IdentityMismatch,
                    WindowsUsbTopologyStage::Correlation,
                    "DEVINST location path differs from the libusb port chain",
                    raw.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            parsed->interface_number.reset();
            if (selected_location.has_value() &&
                *selected_location != *parsed) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::AmbiguousMapping,
                    WindowsUsbTopologyStage::Correlation,
                    "DEVINST stack reports conflicting physical location paths",
                    raw.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            selected_location = std::move(*parsed);
        }
    }

    if (!found_hardware_identity || !selected_location.has_value()) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::IdentityMismatch,
            WindowsUsbTopologyStage::Correlation,
            !found_hardware_identity
                ? "exact DEVINST has no verifiable USB VID/PID identity"
                : "exact DEVINST stack has no matching USB location path",
            snapshot.chain_leaf_to_root.front().device_instance_id_utf8,
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }

    std::ranges::reverse(hubs_leaf_to_root);
    return WindowsUsbTopologyNode{
        .libusb_session_data = query.libusb_session_data,
        .device_instance_id_utf8 =
            snapshot.chain_leaf_to_root.front().device_instance_id_utf8,
        .root_controller_instance_id_utf8 =
            snapshot.chain_leaf_to_root.back().device_instance_id_utf8,
        .hub_instance_ids_utf8 = std::move(hubs_leaf_to_root),
        .location_path_utf8 = format_location_path(*selected_location),
        .root_hub_index = selected_location->root_hub_index,
        // These fields are intentionally retained from one immutable libusb
        // snapshot. Windows PnP bus/address and CompatibleIds are not treated
        // as equivalent to the WinUSB backend's values.
        .vendor_id = query.vendor_id,
        .product_id = query.product_id,
        .bus_number = query.bus_number,
        .device_address = query.device_address,
        .port_numbers = query.port_numbers,
        .serial_utf8 = query.serial_utf8,
        .interface_fingerprint = query.interface_fingerprint,
        .validation_chain_system_nodes = std::move(visited_nodes),
        .validation_chain_instance_ids_utf8 = std::move(visited_ids),
    };
}

#if defined(_WIN32)

[[nodiscard]] std::optional<WindowsUsbTopologyError> native_interrupted(
    const WindowsUsbTopologyStage stage,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token,
    const unsigned long libusb_session_data) {
    return interrupted(stage,
                       deadline,
                       stop_token,
                       system_now,
                       nullptr,
                       libusb_session_data);
}

[[nodiscard]] WindowsUsbTopologyError win32_error(
    const WindowsUsbTopologyStage stage,
    const DWORD native_code,
    std::string message,
    const unsigned long libusb_session_data,
    std::string device_instance_id_utf8 = {}) {
    const auto kind = native_code == ERROR_ACCESS_DENIED
        ? WindowsUsbTopologyErrorKind::PermissionDenied
        : native_code == ERROR_NOT_ENOUGH_MEMORY ||
                native_code == ERROR_OUTOFMEMORY
            ? WindowsUsbTopologyErrorKind::ResourceExhausted
            : WindowsUsbTopologyErrorKind::NativeError;
    return make_error(kind,
                      stage,
                      std::move(message),
                      std::move(device_instance_id_utf8),
                      WindowsUsbNativeErrorDomain::Win32,
                      static_cast<std::uint32_t>(native_code),
                      libusb_session_data);
}

[[nodiscard]] WindowsUsbTopologyError config_error(
    const WindowsUsbTopologyStage stage,
    const CONFIGRET native_code,
    std::string message,
    const unsigned long libusb_session_data,
    std::string device_instance_id_utf8 = {}) {
    const auto kind = native_code == CR_ACCESS_DENIED
        ? WindowsUsbTopologyErrorKind::PermissionDenied
        : native_code == CR_OUT_OF_MEMORY
            ? WindowsUsbTopologyErrorKind::ResourceExhausted
            : WindowsUsbTopologyErrorKind::NativeError;
    return make_error(kind,
                      stage,
                      std::move(message),
                      std::move(device_instance_id_utf8),
                      WindowsUsbNativeErrorDomain::ConfigurationManager,
                      static_cast<std::uint32_t>(native_code),
                      libusb_session_data);
}

[[nodiscard]] bool config_node_missing(const CONFIGRET result) noexcept {
    return result == CR_NO_SUCH_DEVNODE || result == CR_DEVICE_NOT_THERE;
}

[[nodiscard]] std::expected<void, WindowsUsbTopologyError>
require_current_devnode(
    const DEVINST device,
    const WindowsUsbTopologyStage stage,
    const unsigned long libusb_session_data,
    const std::string& device_id,
    const std::string_view operation) {
    ULONG status = 0U;
    ULONG problem = 0U;
    const auto result =
        CM_Get_DevNode_Status(&status, &problem, device, 0U);
    if (config_node_missing(result)) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::IdentityChanged,
            stage,
            std::string{"device disappeared while "} +
                std::string{operation},
            device_id,
            WindowsUsbNativeErrorDomain::ConfigurationManager,
            static_cast<std::uint32_t>(result),
            libusb_session_data));
    }
    if (result != CR_SUCCESS) {
        return std::unexpected(config_error(
            stage,
            result,
            std::string{"failed to verify the device while "} +
                std::string{operation},
            libusb_session_data,
            device_id));
    }
    if ((status & DN_WILL_BE_REMOVED) != 0U ||
        ((status & DN_HAS_PROBLEM) != 0U && problem == CM_PROB_PHANTOM)) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::IdentityChanged,
            stage,
            std::string{"device is no longer present while "} +
                std::string{operation},
            device_id,
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data));
    }
    return {};
}

class DeviceInfoSet final {
public:
    explicit DeviceInfoSet(const HDEVINFO handle) noexcept : handle_(handle) {}
    ~DeviceInfoSet() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)SetupDiDestroyDeviceInfoList(handle_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }

private:
    HDEVINFO handle_{INVALID_HANDLE_VALUE};
};

struct DeviceProperty final {
    DEVPROPTYPE type{};
    std::vector<std::byte> bytes;
};

[[nodiscard]] std::expected<std::optional<DeviceProperty>,
                            WindowsUsbTopologyError>
read_property(const DEVINST device,
              const DEVPROPKEY& key,
              const WindowsUsbTopologyStage stage,
              const unsigned long libusb_session_data,
              const std::string& device_id) {
    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        ULONG required = 0U;
        auto result = CM_Get_DevNode_PropertyW(
            device, &key, &type, nullptr, &required, 0U);
        if (result == CR_NO_SUCH_VALUE) {
            auto current = require_current_devnode(
                device,
                stage,
                libusb_session_data,
                device_id,
                "checking an absent property");
            if (!current.has_value()) {
                return std::unexpected(current.error());
            }
            return std::optional<DeviceProperty>{};
        }
        if (config_node_missing(result)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device disappeared while a property was being sized",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result),
                libusb_session_data));
        }
        if (result != CR_BUFFER_SMALL && result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage,
                result,
                "failed to size a device property",
                libusb_session_data,
                device_id));
        }
        if (required > kMaximumPropertyBytes) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                stage,
                "device property exceeds the bounded snapshot size",
                device_id,
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }
        if (required == 0U) {
            auto current = require_current_devnode(
                device,
                stage,
                libusb_session_data,
                device_id,
                "sizing an empty property");
            if (!current.has_value()) {
                return std::unexpected(current.error());
            }
        }

        DeviceProperty property{
            .type = type,
            .bytes = {},
        };
        property.bytes.resize(required);
        ULONG actual = required;
        result = CM_Get_DevNode_PropertyW(
            device,
            &key,
            &property.type,
            property.bytes.empty()
                ? nullptr
                : reinterpret_cast<PBYTE>(property.bytes.data()),
            &actual,
            0U);
        if (result == CR_BUFFER_SMALL) {
            continue;
        }
        if (result == CR_NO_SUCH_VALUE) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device property disappeared between its size and read",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result),
                libusb_session_data));
        }
        if (config_node_missing(result)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device disappeared while a property was being read",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result),
                libusb_session_data));
        }
        if (result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage,
                result,
                "failed to read a device property",
                libusb_session_data,
                device_id));
        }
        if (actual > required) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device property grew while it was being read",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(CR_BUFFER_SMALL),
                libusb_session_data));
        }
        property.bytes.resize(actual);
        return std::optional<DeviceProperty>{std::move(property)};
    }
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::IdentityChanged,
        stage,
        "device property changed repeatedly while it was being read",
        device_id,
        WindowsUsbNativeErrorDomain::ConfigurationManager,
        static_cast<std::uint32_t>(CR_BUFFER_SMALL),
        libusb_session_data));
}

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
wide_to_utf8(const std::wstring_view value,
             const WindowsUsbTopologyStage stage,
             const unsigned long libusb_session_data,
             const std::string& device_id) {
    if (value.empty() || value.size() > kMaximumIdentityBytes ||
        value.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "Windows device identity text has an invalid size",
            device_id,
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data));
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8,
                                               WC_ERR_INVALID_CHARS,
                                               value.data(),
                                               input_size,
                                               nullptr,
                                               0,
                                               nullptr,
                                               nullptr);
    if (required <= 0) {
        return std::unexpected(win32_error(
            stage,
            GetLastError(),
            "failed to validate UTF-16 device identity text",
            libusb_session_data,
            device_id));
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(CP_UTF8,
                                              WC_ERR_INVALID_CHARS,
                                              value.data(),
                                              input_size,
                                              converted.data(),
                                              required,
                                              nullptr,
                                              nullptr);
    if (written != required || !valid_identity_text(converted)) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "Windows device identity text is not valid UTF-8",
            device_id,
            WindowsUsbNativeErrorDomain::Win32,
            written == 0 ? static_cast<std::uint32_t>(GetLastError()) : 0U,
            libusb_session_data));
    }
    return converted;
}

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
device_instance_id(const DEVINST device,
                   const WindowsUsbTopologyStage stage,
                   const unsigned long libusb_session_data) {
    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
        ULONG characters = 0U;
        auto result = CM_Get_Device_ID_Size(&characters, device, 0U);
        if (config_node_missing(result)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "the libusb DEVINST is no longer valid",
                {},
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result),
                libusb_session_data));
        }
        if (result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage,
                result,
                "failed to size a device instance identifier",
                libusb_session_data));
        }
        if (characters == 0U || characters >= kMaximumIdentityBytes) {
            if (characters == 0U) {
                auto current = require_current_devnode(
                    device,
                    stage,
                    libusb_session_data,
                    {},
                    "sizing an empty instance identifier");
                if (!current.has_value()) {
                    return std::unexpected(current.error());
                }
            }
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                stage,
                "device instance identifier has an invalid size",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(characters) + 1U);
        result = CM_Get_Device_IDW(
            device, buffer.data(), static_cast<ULONG>(buffer.size()), 0U);
        if (result == CR_BUFFER_SMALL) {
            continue;
        }
        if (config_node_missing(result)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "the libusb DEVINST disappeared while its instance identifier was read",
                {},
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result),
                libusb_session_data));
        }
        if (result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage,
                result,
                "failed to read a device instance identifier",
                libusb_session_data));
        }
        const auto end = std::ranges::find(buffer, L'\0');
        if (end == buffer.end()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                stage,
                "device instance identifier is not terminated",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }
        return wide_to_utf8(
            std::wstring_view(buffer.data(),
                              static_cast<std::size_t>(end - buffer.begin())),
            stage,
            libusb_session_data,
            {});
    }
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::IdentityChanged,
        stage,
        "device instance identifier changed repeatedly while it was read",
        {},
        WindowsUsbNativeErrorDomain::ConfigurationManager,
        static_cast<std::uint32_t>(CR_BUFFER_SMALL),
        libusb_session_data));
}

[[nodiscard]] std::expected<std::vector<std::string>,
                            WindowsUsbTopologyError>
property_string_list(const DEVINST device,
                     const DEVPROPKEY& key,
                     const WindowsUsbTopologyStage stage,
                     const unsigned long libusb_session_data,
                     const std::string& device_id) {
    auto property =
        read_property(device, key, stage, libusb_session_data, device_id);
    if (!property.has_value()) {
        return std::unexpected(property.error());
    }
    if (!property->has_value()) {
        return std::vector<std::string>{};
    }
    if ((*property)->type != DEVPROP_TYPE_STRING_LIST ||
        (*property)->bytes.size() < 2U * sizeof(wchar_t) ||
        (*property)->bytes.size() % sizeof(wchar_t) != 0U) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "device string-list property has an unexpected type or size",
            device_id,
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data));
    }
    std::vector<wchar_t> text((*property)->bytes.size() / sizeof(wchar_t));
    std::memcpy(text.data(), (*property)->bytes.data(), (*property)->bytes.size());
    if (text.size() < 2U || text[text.size() - 1U] != L'\0' ||
        text[text.size() - 2U] != L'\0') {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "device string-list property is not doubly terminated",
            device_id,
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data));
    }

    std::vector<std::string> values;
    std::size_t start = 0U;
    while (start + 1U < text.size()) {
        const auto end = std::ranges::find(
            text.begin() + static_cast<std::ptrdiff_t>(start),
            text.end(),
            L'\0');
        const auto length = static_cast<std::size_t>(
            end - (text.begin() + static_cast<std::ptrdiff_t>(start)));
        if (length == 0U) {
            if (!std::ranges::all_of(
                    text.begin() + static_cast<std::ptrdiff_t>(start),
                    text.end(),
                    [](const wchar_t character) { return character == L'\0'; })) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    stage,
                    "device string-list has data after its terminator",
                    device_id,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    libusb_session_data));
            }
            break;
        }
        auto converted = wide_to_utf8(
            std::wstring_view(text.data() + start, length),
            stage,
            libusb_session_data,
            device_id);
        if (!converted.has_value()) {
            return std::unexpected(converted.error());
        }
        values.push_back(std::move(*converted));
        start += length + 1U;
    }
    return values;
}

[[nodiscard]] std::expected<std::vector<unsigned long>,
                            WindowsUsbTopologyError>
enumerate_present_device_nodes(
    const GUID* const interface_class,
    const DWORD flags,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token,
    const unsigned long libusb_session_data) {
    DeviceInfoSet devices(SetupDiGetClassDevsW(
        interface_class,
        nullptr,
        nullptr,
        flags));
    if (devices.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(win32_error(
            WindowsUsbTopologyStage::Enumeration,
            GetLastError(),
            "failed to enumerate a present Windows device set",
            libusb_session_data));
    }

    std::vector<unsigned long> result;
    for (DWORD index = 0U;; ++index) {
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::Enumeration,
                deadline,
                stop_token,
                libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        SP_DEVINFO_DATA data{};
        data.cbSize = static_cast<DWORD>(sizeof(data));
        if (SetupDiEnumDeviceInfo(devices.get(), index, &data) == FALSE) {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) {
                break;
            }
            return std::unexpected(win32_error(
                WindowsUsbTopologyStage::Enumeration,
                error,
                "present Windows devnode enumeration failed",
                libusb_session_data));
        }
        const auto system_node = static_cast<unsigned long>(data.DevInst);
        if (system_node == 0UL ||
            std::ranges::find(result, system_node) != result.end()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::AmbiguousMapping,
                WindowsUsbTopologyStage::Enumeration,
                "present Windows device set contains a duplicate DEVINST",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }
        result.push_back(system_node);
    }
    return result;
}

[[nodiscard]] bool contains_system_node(
    const std::vector<unsigned long>& nodes,
    const unsigned long value) noexcept {
    return std::ranges::find(nodes, value) != nodes.end();
}

#endif  // defined(_WIN32)

}  // namespace

std::expected<WindowsUsbLocationPath, WindowsUsbTopologyError>
parse_windows_usb_location_path(const std::string_view location_path_utf8) {
    try {
        if (!valid_identity_text(location_path_utf8)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::InvalidArgument,
                WindowsUsbTopologyStage::Validation,
                "Windows USB location path is not bounded valid UTF-8"));
        }

        constexpr std::string_view root_prefix = "USBROOT(";
        constexpr std::string_view port_prefix = "USB(";
        constexpr std::string_view interface_prefix = "USBMI(";
        WindowsUsbLocationPath result;
        bool found_root = false;
        bool found_interface = false;
        std::size_t start = 0U;
        while (start <= location_path_utf8.size()) {
            const auto separator = location_path_utf8.find('#', start);
            const auto component = location_path_utf8.substr(
                start,
                separator == std::string_view::npos
                    ? std::string_view::npos
                    : separator - start);
            if (component.empty()) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::Correlation,
                    "Windows USB location path contains an empty component"));
            }
            if (!found_root && ascii_istarts_with(component, root_prefix) &&
                component.back() == ')') {
                const auto root_index = parse_decimal<std::uint32_t>(
                    component.substr(root_prefix.size(),
                                     component.size() - root_prefix.size() - 1U));
                if (!root_index.has_value() ||
                    result.controller_prefix_utf8.empty()) {
                    return std::unexpected(make_error(
                        WindowsUsbTopologyErrorKind::MalformedSnapshot,
                        WindowsUsbTopologyStage::Correlation,
                        "Windows USB root location component is malformed"));
                }
                result.root_hub_index = *root_index;
                found_root = true;
            } else if (found_root && !found_interface &&
                       ascii_istarts_with(component, port_prefix) &&
                       component.back() == ')') {
                const auto port = parse_decimal<std::uint8_t>(
                    component.substr(port_prefix.size(),
                                     component.size() - port_prefix.size() - 1U));
                if (!port.has_value() || *port == 0U) {
                    return std::unexpected(make_error(
                        WindowsUsbTopologyErrorKind::MalformedSnapshot,
                        WindowsUsbTopologyStage::Correlation,
                        "Windows USB port location component is malformed"));
                }
                if (result.port_numbers.size() ==
                    kMaximumWindowsUsbTopologyDepth) {
                    return std::unexpected(make_error(
                        WindowsUsbTopologyErrorKind::TopologyTooDeep,
                        WindowsUsbTopologyStage::Correlation,
                        "Windows USB location path exceeds the supported depth"));
                }
                result.port_numbers.push_back(*port);
            } else if (found_root && !found_interface &&
                       ascii_istarts_with(component, interface_prefix) &&
                       component.back() == ')' &&
                       separator == std::string_view::npos) {
                const auto interface_number = parse_decimal<std::uint8_t>(
                    component.substr(interface_prefix.size(),
                                     component.size() -
                                         interface_prefix.size() - 1U));
                if (!interface_number.has_value()) {
                    return std::unexpected(make_error(
                        WindowsUsbTopologyErrorKind::MalformedSnapshot,
                        WindowsUsbTopologyStage::Correlation,
                        "Windows USB interface location component is malformed"));
                }
                result.interface_number = *interface_number;
                found_interface = true;
            } else if (found_root) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::Correlation,
                    "Windows USB location path has an unsupported component after its root"));
            } else {
                if (!result.controller_prefix_utf8.empty()) {
                    result.controller_prefix_utf8.push_back('#');
                }
                result.controller_prefix_utf8.append(component);
            }
            if (separator == std::string_view::npos) {
                break;
            }
            start = separator + 1U;
        }
        if (!found_root || result.port_numbers.empty()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                WindowsUsbTopologyStage::Correlation,
                "Windows location path does not identify a USB port chain"));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Correlation,
            "memory allocation failed while parsing a Windows USB location path"));
    }
}

std::expected<std::string, WindowsUsbTopologyError>
canonical_windows_usb_port_path(
    const std::uint8_t bus_number,
    const std::vector<std::uint8_t>& port_numbers) {
    try {
        if (bus_number == 0U || port_numbers.empty() ||
            std::ranges::any_of(port_numbers, [](const std::uint8_t port) {
                return port == 0U;
            })) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::InvalidArgument,
                WindowsUsbTopologyStage::Validation,
                "USB physical path requires a non-zero bus and port chain"));
        }
        if (port_numbers.size() > kMaximumWindowsUsbTopologyDepth) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::TopologyTooDeep,
                WindowsUsbTopologyStage::Validation,
                "USB physical path exceeds the supported topology depth"));
        }
        std::string result = "usb:" + std::to_string(bus_number) + "-";
        for (std::size_t index = 0U; index < port_numbers.size(); ++index) {
            if (index != 0U) {
                result.push_back('.');
            }
            result += std::to_string(port_numbers[index]);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Correlation,
            "memory allocation failed while formatting a USB physical path"));
    }
}

std::expected<std::string, WindowsUsbTopologyError>
read_windows_usb_session_instance_id(
    const unsigned long libusb_session_data,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) {
#if !defined(_WIN32)
    (void)deadline;
    (void)stop_token;
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::UnsupportedPlatform,
        WindowsUsbTopologyStage::Enumeration,
        "Windows PnP session identity is only available on Windows",
        {},
        WindowsUsbNativeErrorDomain::None,
        0U,
        libusb_session_data));
#else
    try {
        static_assert(sizeof(DEVINST) == sizeof(unsigned long));
        if (libusb_session_data == 0UL) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::InvalidArgument,
                WindowsUsbTopologyStage::Validation,
                "libusb WinUSB session data must contain a non-zero DEVINST"));
        }
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::Enumeration,
                deadline,
                stop_token,
                libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        const auto device = static_cast<DEVINST>(libusb_session_data);
        ULONG status = 0U;
        ULONG problem = 0U;
        const auto status_result =
            CM_Get_DevNode_Status(&status, &problem, device, 0U);
        if (config_node_missing(status_result)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::Enumeration,
                "the libusb DEVINST disappeared before its PnP generation was captured",
                {},
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(status_result),
                libusb_session_data));
        }
        if (status_result != CR_SUCCESS) {
            return std::unexpected(config_error(
                WindowsUsbTopologyStage::Enumeration,
                status_result,
                "failed to query the libusb DEVINST generation status",
                libusb_session_data));
        }
        if ((status & DN_WILL_BE_REMOVED) != 0U ||
            ((status & DN_HAS_PROBLEM) != 0U &&
             problem == CM_PROB_PHANTOM)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::Enumeration,
                "the libusb DEVINST generation is no longer present",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }

        auto first = device_instance_id(device,
                                        WindowsUsbTopologyStage::Enumeration,
                                        libusb_session_data);
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::Enumeration,
                deadline,
                stop_token,
                libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!first.has_value()) {
            return std::unexpected(first.error());
        }
        auto second = device_instance_id(device,
                                         WindowsUsbTopologyStage::Enumeration,
                                         libusb_session_data);
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::Enumeration,
                deadline,
                stop_token,
                libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!second.has_value()) {
            return std::unexpected(second.error());
        }
        if (!ascii_iequals(*first, *second)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::Enumeration,
                "the PnP instance identity changed while it was captured",
                *second,
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }
        return first;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Enumeration,
            "memory allocation failed while capturing the PnP generation",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data));
    }
#endif
}

std::expected<std::vector<WindowsUsbNativeSnapshotResult>,
              WindowsUsbTopologyError>
IWindowsUsbTopologyNativeBackend::read_snapshots(
    const std::span<const unsigned long> libusb_session_data,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
    std::vector<WindowsUsbNativeSnapshotResult> results;
    results.reserve(libusb_session_data.size());
    for (const auto session : libusb_session_data) {
        results.push_back(read_snapshot(session, deadline, stop_token));
    }
    return results;
}

#if defined(_WIN32)
[[nodiscard]] std::expected<WindowsUsbNativeSnapshot,
                            WindowsUsbTopologyError>
read_snapshot_from_global_nodes(
    const unsigned long libusb_session_data,
    const std::vector<unsigned long>& present_nodes,
    const std::vector<unsigned long>& hub_nodes,
    const std::vector<unsigned long>& controller_nodes,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) {
    WindowsUsbNativeSnapshot snapshot{
        .requested_system_node = libusb_session_data,
        .chain_leaf_to_root = {},
    };
    auto current = static_cast<DEVINST>(libusb_session_data);
    for (std::size_t depth = 0U; depth < kMaximumNativeParentDepth; ++depth) {
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::ParentTraversal,
                deadline,
                stop_token,
                libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        ULONG status = 0U;
        ULONG problem = 0U;
        const auto current_value = static_cast<unsigned long>(current);
        if (!contains_system_node(present_nodes, current_value)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::ParentTraversal,
                "a DEVINST in the libusb parent chain is no longer present",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }
        const auto status_result =
            CM_Get_DevNode_Status(&status, &problem, current, 0U);
        if (config_node_missing(status_result)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::ParentTraversal,
                "the libusb DEVINST disappeared during parent traversal",
                {},
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(status_result),
                libusb_session_data));
        }
        if (status_result != CR_SUCCESS) {
            return std::unexpected(config_error(
                WindowsUsbTopologyStage::ParentTraversal,
                status_result,
                "failed to query DEVINST status",
                libusb_session_data));
        }
        if ((status & DN_WILL_BE_REMOVED) != 0U ||
            ((status & DN_HAS_PROBLEM) != 0U && problem == CM_PROB_PHANTOM)) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::ParentTraversal,
                "a DEVINST in the libusb parent chain is being removed",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                libusb_session_data));
        }

        auto id = device_instance_id(
            current,
            WindowsUsbTopologyStage::ParentTraversal,
            libusb_session_data);
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        auto hardware_ids = property_string_list(
            current,
            DEVPKEY_Device_HardwareIds,
            WindowsUsbTopologyStage::PropertyRead,
            libusb_session_data,
            *id);
        if (!hardware_ids.has_value()) {
            return std::unexpected(hardware_ids.error());
        }
        auto location_paths = property_string_list(
            current,
            DEVPKEY_Device_LocationPaths,
            WindowsUsbTopologyStage::PropertyRead,
            libusb_session_data,
            *id);
        if (!location_paths.has_value()) {
            return std::unexpected(location_paths.error());
        }

        const bool host_controller =
            contains_system_node(controller_nodes, current_value);
        std::optional<unsigned long> parent_value;
        DEVINST parent = 0U;
        if (!host_controller) {
            const auto parent_result = CM_Get_Parent(&parent, current, 0U);
            if (config_node_missing(parent_result)) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::IdentityChanged,
                    WindowsUsbTopologyStage::ParentTraversal,
                    "DEVINST parent disappeared during traversal",
                    *id,
                    WindowsUsbNativeErrorDomain::ConfigurationManager,
                    static_cast<std::uint32_t>(parent_result),
                    libusb_session_data));
            }
            if (parent_result != CR_SUCCESS) {
                return std::unexpected(config_error(
                    WindowsUsbTopologyStage::ParentTraversal,
                    parent_result,
                    "failed to read a DEVINST parent",
                    libusb_session_data,
                    *id));
            }
            parent_value = static_cast<unsigned long>(parent);
        }

        snapshot.chain_leaf_to_root.push_back(
            WindowsUsbNativeNodeSnapshot{
                .system_node = current_value,
                .parent_system_node = parent_value,
                .device_instance_id_utf8 = std::move(*id),
                .present = true,
                .exposes_usb_hub_interface =
                    contains_system_node(hub_nodes, current_value),
                .exposes_usb_host_controller_interface = host_controller,
                .hardware_ids_utf8 = std::move(*hardware_ids),
                .location_paths_utf8 = std::move(*location_paths),
            });
        if (host_controller) {
            return snapshot;
        }
        current = parent;
    }
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::TopologyTooDeep,
        WindowsUsbTopologyStage::ParentTraversal,
        "Windows devnode ancestry does not reach a USB host controller",
        {},
        WindowsUsbNativeErrorDomain::None,
        0U,
        libusb_session_data));
}
#endif

std::expected<std::vector<WindowsUsbNativeSnapshotResult>,
              WindowsUsbTopologyError>
SetupApiWindowsUsbNativeBackend::read_snapshots(
    const std::span<const unsigned long> libusb_session_data,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
#if !defined(_WIN32)
    (void)deadline;
    (void)stop_token;
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::UnsupportedPlatform,
        WindowsUsbTopologyStage::Enumeration,
        "SetupAPI USB topology discovery is only available on Windows",
        {},
        WindowsUsbNativeErrorDomain::None,
        0U,
        libusb_session_data.empty() ? 0UL : libusb_session_data.front()));
#else
    try {
        static_assert(sizeof(DEVINST) == sizeof(unsigned long));
        std::vector<unsigned long> sessions;
        sessions.reserve(libusb_session_data.size());
        for (const auto session : libusb_session_data) {
            if (session == 0UL ||
                std::ranges::find(sessions, session) != sessions.end()) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::InvalidArgument,
                    WindowsUsbTopologyStage::Validation,
                    "Windows topology batch requires unique non-zero DEVINST values",
                    {},
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    session));
            }
            sessions.push_back(session);
        }
        if (sessions.empty()) {
            return std::vector<WindowsUsbNativeSnapshotResult>{};
        }

        const auto correlation_session = sessions.front();
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::Enumeration,
                deadline,
                stop_token,
                correlation_session);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        auto present_nodes = enumerate_present_device_nodes(
            nullptr,
            DIGCF_PRESENT | DIGCF_ALLCLASSES,
            deadline,
            stop_token,
            correlation_session);
        if (!present_nodes.has_value()) {
            return std::unexpected(present_nodes.error());
        }
        auto hub_nodes = enumerate_present_device_nodes(
            &GUID_DEVINTERFACE_USB_HUB,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
            deadline,
            stop_token,
            correlation_session);
        if (!hub_nodes.has_value()) {
            return std::unexpected(hub_nodes.error());
        }
        auto controller_nodes = enumerate_present_device_nodes(
            &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
            deadline,
            stop_token,
            correlation_session);
        if (!controller_nodes.has_value()) {
            return std::unexpected(controller_nodes.error());
        }

        std::vector<WindowsUsbNativeSnapshotResult> results;
        results.reserve(sessions.size());
        for (const auto session : sessions) {
            auto snapshot = read_snapshot_from_global_nodes(
                session,
                *present_nodes,
                *hub_nodes,
                *controller_nodes,
                deadline,
                stop_token);
            if (!snapshot.has_value() &&
                (snapshot.error().kind == WindowsUsbTopologyErrorKind::Cancelled ||
                 snapshot.error().kind == WindowsUsbTopologyErrorKind::TimedOut ||
                 snapshot.error().kind ==
                     WindowsUsbTopologyErrorKind::ResourceExhausted)) {
                return std::unexpected(snapshot.error());
            }
            results.push_back(std::move(snapshot));
        }
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::StabilityCheck,
                deadline,
                stop_token,
                correlation_session);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        return results;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Enumeration,
            "memory allocation failed during SetupAPI USB batch enumeration",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data.empty() ? 0UL : libusb_session_data.front()));
    }
#endif
}

std::expected<WindowsUsbNativeSnapshot, WindowsUsbTopologyError>
SetupApiWindowsUsbNativeBackend::read_snapshot(
    const unsigned long libusb_session_data,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
    const std::array sessions{libusb_session_data};
    auto results = read_snapshots(sessions, deadline, stop_token);
    if (!results.has_value()) {
        return std::unexpected(results.error());
    }
    if (results->size() != 1U) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            WindowsUsbTopologyStage::Enumeration,
            "Windows native batch returned the wrong result count",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            libusb_session_data));
    }
    return std::move(results->front());
}

SetupApiWindowsUsbTopologyBackend::SetupApiWindowsUsbTopologyBackend(
    const IWindowsUsbTopologyNativeBackend& native_backend) noexcept
    : native_backend_(&native_backend) {}

std::expected<std::vector<WindowsUsbTopologyNode>, WindowsUsbTopologyError>
SetupApiWindowsUsbTopologyBackend::read_candidates(
    const WindowsUsbTopologyQuery& query,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
    const std::array queries{query};
    auto batch = read_candidate_batch(queries, deadline, stop_token);
    if (!batch.has_value()) {
        return std::unexpected(batch.error());
    }
    if (batch->size() != 1U) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            WindowsUsbTopologyStage::Enumeration,
            "Windows topology batch returned the wrong device count",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }
    if (!batch->front().has_value()) {
        return std::unexpected(batch->front().error());
    }
    return std::move(batch->front()->interface_candidates);
}

std::expected<std::vector<WindowsUsbTopologyDeviceCandidatesResult>,
              WindowsUsbTopologyError>
SetupApiWindowsUsbTopologyBackend::read_candidate_batch(
    const std::span<const WindowsUsbTopologyQuery> queries,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
    try {
        struct QueryGroup final {
            unsigned long session{};
            std::vector<const WindowsUsbTopologyQuery*> queries;
        };
        std::vector<QueryGroup> groups;
        for (const auto& query : queries) {
            if (!query_is_valid(query)) {
                return std::unexpected(make_error(
                    query.port_numbers.size() >
                            kMaximumWindowsUsbTopologyDepth
                        ? WindowsUsbTopologyErrorKind::TopologyTooDeep
                        : WindowsUsbTopologyErrorKind::InvalidArgument,
                    WindowsUsbTopologyStage::Validation,
                    "invalid libusb identity for Windows topology batch discovery",
                    {},
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            auto group = std::ranges::find(
                groups, query.libusb_session_data, &QueryGroup::session);
            if (group == groups.end()) {
                groups.push_back(QueryGroup{
                    .session = query.libusb_session_data,
                    .queries = {&query},
                });
                continue;
            }
            const auto& first = *group->queries.front();
            if (!ascii_iequals(first.device_instance_id_utf8,
                               query.device_instance_id_utf8) ||
                first.vendor_id != query.vendor_id ||
                first.product_id != query.product_id ||
                first.bus_number != query.bus_number ||
                first.device_address != query.device_address ||
                first.port_numbers != query.port_numbers ||
                first.serial_utf8 != query.serial_utf8 ||
                std::ranges::any_of(
                    group->queries,
                    [&query](const WindowsUsbTopologyQuery* existing) {
                        return existing->interface_fingerprint ==
                            query.interface_fingerprint;
                    })) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::InvalidArgument,
                    WindowsUsbTopologyStage::Validation,
                    "queries for one Windows DEVINST must describe one device and distinct interfaces",
                    query.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            group->queries.push_back(&query);
        }
        if (groups.empty()) {
            return std::vector<WindowsUsbTopologyDeviceCandidatesResult>{};
        }

        SetupApiWindowsUsbNativeBackend production_native;
        const auto& native = native_backend_ == nullptr
            ? static_cast<const IWindowsUsbTopologyNativeBackend&>(
                  production_native)
            : *native_backend_;
        std::vector<unsigned long> sessions;
        sessions.reserve(groups.size());
        for (const auto& group : groups) {
            sessions.push_back(group.session);
        }
        auto snapshots = native.read_snapshots(sessions, deadline, stop_token);
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::StabilityCheck,
                deadline,
                stop_token,
                system_now,
                nullptr,
                sessions.front());
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!snapshots.has_value()) {
            return std::unexpected(snapshots.error());
        }
        if (snapshots->size() != groups.size()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                WindowsUsbTopologyStage::Enumeration,
                "Windows native batch returned the wrong device count",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                sessions.front()));
        }

        std::vector<WindowsUsbTopologyDeviceCandidatesResult> results;
        results.reserve(groups.size());
        for (std::size_t index = 0U; index < groups.size(); ++index) {
            const auto& group = groups[index];
            auto& snapshot = (*snapshots)[index];
            if (!snapshot.has_value()) {
                if (snapshot.error().libusb_session_data != group.session) {
                    return std::unexpected(make_error(
                        WindowsUsbTopologyErrorKind::MalformedSnapshot,
                        WindowsUsbTopologyStage::Enumeration,
                        "Windows native batch error is out of order",
                        snapshot.error().device_instance_id_utf8,
                        WindowsUsbNativeErrorDomain::None,
                        0U,
                        group.session));
                }
                results.push_back(std::unexpected(snapshot.error()));
                continue;
            }
            if (snapshot->requested_system_node != group.session) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::IdentityMismatch,
                    WindowsUsbTopologyStage::Enumeration,
                    "Windows native batch snapshot is out of order",
                    {},
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    group.session));
            }
            std::vector<WindowsUsbTopologyNode> candidates;
            candidates.reserve(group.queries.size());
            std::optional<WindowsUsbTopologyError> device_error;
            for (const auto* query : group.queries) {
                auto candidate = map_native_snapshot(
                    *query, *snapshot, deadline, stop_token);
                if (!candidate.has_value()) {
                    device_error = candidate.error();
                    break;
                }
                candidates.push_back(std::move(*candidate));
            }
            if (device_error.has_value()) {
                results.push_back(std::unexpected(std::move(*device_error)));
                continue;
            }
            results.push_back(WindowsUsbTopologyDeviceCandidates{
                .libusb_session_data = group.session,
                .native_snapshot = std::move(*snapshot),
                .interface_candidates = std::move(candidates),
            });
        }
        return results;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Enumeration,
            "memory allocation failed during Windows USB batch correlation",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            queries.empty() ? 0UL : queries.front().libusb_session_data));
    }
}

WindowsUsbTopologyDiscovery::WindowsUsbTopologyDiscovery(
    const IWindowsUsbTopologyBackend& backend,
    const WindowsUsbTopologyNow now,
    void* const now_context) noexcept
    : backend_(backend),
      now_(now == nullptr ? system_now : now),
      now_context_(now_context) {}

std::expected<WindowsUsbTopology, WindowsUsbTopologyError>
WindowsUsbTopologyDiscovery::discover(
    const WindowsUsbTopologyQuery& query,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
    try {
        if (!query_is_valid(query)) {
            return std::unexpected(make_error(
                query.port_numbers.size() > kMaximumWindowsUsbTopologyDepth
                    ? WindowsUsbTopologyErrorKind::TopologyTooDeep
                    : WindowsUsbTopologyErrorKind::InvalidArgument,
                WindowsUsbTopologyStage::Validation,
                "invalid libusb identity for Windows topology discovery",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }
        if (const auto stop = interrupted(WindowsUsbTopologyStage::Validation,
                                          deadline,
                                          stop_token,
                                          now_,
                                          now_context_,
                                          query.libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        auto first = backend_.read_candidates(query, deadline, stop_token);
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::StabilityCheck,
                deadline,
                stop_token,
                now_,
                now_context_,
                query.libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!first.has_value()) {
            return std::unexpected(first.error());
        }
        auto second = backend_.read_candidates(query, deadline, stop_token);
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::StabilityCheck,
                deadline,
                stop_token,
                now_,
                now_context_,
                query.libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!second.has_value()) {
            return std::unexpected(second.error());
        }

        std::ranges::sort(*first, node_less);
        std::ranges::sort(*second, node_less);
        if (*first != *second) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::StabilityCheck,
                "Windows USB topology changed between validation snapshots",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }

        const WindowsUsbTopologyNode* match = nullptr;
        bool identity_mismatch = false;
        for (const auto& candidate : *second) {
            if (const auto stop = interrupted(
                    WindowsUsbTopologyStage::Correlation,
                    deadline,
                    stop_token,
                    now_,
                    now_context_,
                    query.libusb_session_data);
                stop.has_value()) {
                return std::unexpected(*stop);
            }
            if (!node_shape_is_valid(candidate)) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::Correlation,
                    "Windows backend returned a malformed USB topology node",
                    candidate.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            if (candidate.libusb_session_data != query.libusb_session_data) {
                continue;
            }
            if (!ascii_iequals(candidate.device_instance_id_utf8,
                               query.device_instance_id_utf8)) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::IdentityChanged,
                    WindowsUsbTopologyStage::Correlation,
                    "the libusb DEVINST was reused by another PnP device generation",
                    candidate.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            if (candidate.vendor_id != query.vendor_id ||
                candidate.product_id != query.product_id ||
                candidate.bus_number != query.bus_number ||
                candidate.device_address != query.device_address ||
                candidate.port_numbers != query.port_numbers ||
                candidate.interface_fingerprint !=
                    query.interface_fingerprint ||
                candidate.serial_utf8 != query.serial_utf8) {
                identity_mismatch = true;
                continue;
            }
            if (match != nullptr) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::AmbiguousMapping,
                    WindowsUsbTopologyStage::Correlation,
                    "more than one topology maps to the exact libusb DEVINST",
                    candidate.device_instance_id_utf8,
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            match = &candidate;
        }
        if (match == nullptr) {
            return std::unexpected(make_error(
                identity_mismatch
                    ? WindowsUsbTopologyErrorKind::IdentityMismatch
                    : WindowsUsbTopologyErrorKind::NotFound,
                WindowsUsbTopologyStage::Correlation,
                identity_mismatch
                    ? "Windows USB identity does not match the libusb snapshot"
                    : "the exact libusb DEVINST has no present Windows topology",
                {},
                WindowsUsbNativeErrorDomain::None,
                0U,
                query.libusb_session_data));
        }

        auto physical_path = canonical_windows_usb_port_path(
            match->bus_number, match->port_numbers);
        if (!physical_path.has_value()) {
            return std::unexpected(physical_path.error());
        }
        return WindowsUsbTopology{
            .physical_port_path = std::move(*physical_path),
            .root_controller_id =
                "windows-pnp:" + match->root_controller_instance_id_utf8,
            .hub_port_chain = match->port_numbers,
            .vendor_id = match->vendor_id,
            .product_id = match->product_id,
            .bus_number = match->bus_number,
            .device_address = match->device_address,
            .serial_utf8 = match->serial_utf8,
            .interface_fingerprint = match->interface_fingerprint,
            .device_instance_id_utf8 = match->device_instance_id_utf8,
            .hub_instance_ids_utf8 = match->hub_instance_ids_utf8,
            .location_path_utf8 = match->location_path_utf8,
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Correlation,
            "memory allocation failed while correlating Windows USB topology",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            query.libusb_session_data));
    }
}

std::expected<std::vector<WindowsUsbTopologyResult>,
              WindowsUsbTopologyError>
WindowsUsbTopologyDiscovery::discover_batch(
    const std::span<const WindowsUsbTopologyQuery> queries,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
    try {
        struct QueryGroup final {
            unsigned long session{};
            std::vector<std::size_t> query_indices;
        };
        std::vector<QueryGroup> groups;
        for (std::size_t index = 0U; index < queries.size(); ++index) {
            const auto& query = queries[index];
            if (!query_is_valid(query)) {
                return std::unexpected(make_error(
                    query.port_numbers.size() >
                            kMaximumWindowsUsbTopologyDepth
                        ? WindowsUsbTopologyErrorKind::TopologyTooDeep
                        : WindowsUsbTopologyErrorKind::InvalidArgument,
                    WindowsUsbTopologyStage::Validation,
                    "invalid libusb identity for Windows topology batch discovery",
                    {},
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    query.libusb_session_data));
            }
            auto group = std::ranges::find(
                groups, query.libusb_session_data, &QueryGroup::session);
            if (group == groups.end()) {
                groups.push_back(QueryGroup{
                    .session = query.libusb_session_data,
                    .query_indices = {index},
                });
            } else {
                group->query_indices.push_back(index);
            }
        }
        if (queries.empty()) {
            return std::vector<WindowsUsbTopologyResult>{};
        }
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::Validation,
                deadline,
                stop_token,
                now_,
                now_context_,
                queries.front().libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        auto first = backend_.read_candidate_batch(
            queries, deadline, stop_token);
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::StabilityCheck,
                deadline,
                stop_token,
                now_,
                now_context_,
                queries.front().libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!first.has_value()) {
            return std::unexpected(first.error());
        }
        auto second = backend_.read_candidate_batch(
            queries, deadline, stop_token);
        if (const auto stop = interrupted(
                WindowsUsbTopologyStage::StabilityCheck,
                deadline,
                stop_token,
                now_,
                now_context_,
                queries.front().libusb_session_data);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        if (!second.has_value()) {
            return std::unexpected(second.error());
        }

        const auto validate_shape = [&groups](
            const std::vector<WindowsUsbTopologyDeviceCandidatesResult>& batch)
            -> std::optional<WindowsUsbTopologyError> {
            if (batch.size() != groups.size()) {
                return make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::StabilityCheck,
                    "Windows topology batch returned the wrong device count");
            }
            for (std::size_t index = 0U; index < groups.size(); ++index) {
                const auto& group = groups[index];
                const auto& result = batch[index];
                if (!result.has_value()) {
                    if (result.error().libusb_session_data != group.session) {
                        return make_error(
                            WindowsUsbTopologyErrorKind::MalformedSnapshot,
                            WindowsUsbTopologyStage::StabilityCheck,
                            "Windows topology batch error is out of order",
                            result.error().device_instance_id_utf8,
                            WindowsUsbNativeErrorDomain::None,
                            0U,
                            group.session);
                    }
                    continue;
                }
                if (result->libusb_session_data != group.session ||
                    result->native_snapshot.requested_system_node !=
                        group.session ||
                    result->interface_candidates.size() !=
                        group.query_indices.size()) {
                    return make_error(
                        WindowsUsbTopologyErrorKind::MalformedSnapshot,
                        WindowsUsbTopologyStage::StabilityCheck,
                        "Windows topology batch device result is out of order or incomplete",
                        {},
                        WindowsUsbNativeErrorDomain::None,
                        0U,
                        group.session);
                }
            }
            return std::nullopt;
        };
        if (const auto malformed = validate_shape(*first);
            malformed.has_value()) {
            return std::unexpected(*malformed);
        }
        if (const auto malformed = validate_shape(*second);
            malformed.has_value()) {
            return std::unexpected(*malformed);
        }

        std::vector<std::optional<WindowsUsbTopologyResult>> staged(
            queries.size());
        for (std::size_t group_index = 0U;
             group_index < groups.size();
             ++group_index) {
            const auto& group = groups[group_index];
            const auto& first_result = (*first)[group_index];
            const auto& second_result = (*second)[group_index];
            std::optional<WindowsUsbTopologyError> device_error;
            if (!first_result.has_value() || !second_result.has_value()) {
                const auto& error = !second_result.has_value()
                    ? second_result.error()
                    : first_result.error();
                if (error.kind == WindowsUsbTopologyErrorKind::Cancelled ||
                    error.kind == WindowsUsbTopologyErrorKind::TimedOut ||
                    error.kind ==
                        WindowsUsbTopologyErrorKind::ResourceExhausted) {
                    return std::unexpected(error);
                }
                if (!first_result.has_value() &&
                    !second_result.has_value() &&
                    first_result.error() == second_result.error()) {
                    device_error = error;
                } else {
                    device_error = make_error(
                        WindowsUsbTopologyErrorKind::IdentityChanged,
                        WindowsUsbTopologyStage::StabilityCheck,
                        "Windows USB topology device result changed between global snapshots",
                        error.device_instance_id_utf8,
                        WindowsUsbNativeErrorDomain::None,
                        0U,
                        group.session);
                }
            } else if (*first_result != *second_result) {
                device_error = make_error(
                    WindowsUsbTopologyErrorKind::IdentityChanged,
                    WindowsUsbTopologyStage::StabilityCheck,
                    "Windows USB topology changed between global validation snapshots",
                    {},
                    WindowsUsbNativeErrorDomain::None,
                    0U,
                    group.session);
            }

            std::vector<WindowsUsbTopology> resolved;
            if (!device_error.has_value()) {
                resolved.reserve(group.query_indices.size());
                for (std::size_t offset = 0U;
                     offset < group.query_indices.size();
                     ++offset) {
                    const auto query_index = group.query_indices[offset];
                    const auto& query = queries[query_index];
                    const auto& candidate =
                        second_result->interface_candidates[offset];
                    if (!node_shape_is_valid(candidate) ||
                        candidate.libusb_session_data !=
                            query.libusb_session_data ||
                        !ascii_iequals(candidate.device_instance_id_utf8,
                                       query.device_instance_id_utf8) ||
                        candidate.vendor_id != query.vendor_id ||
                        candidate.product_id != query.product_id ||
                        candidate.bus_number != query.bus_number ||
                        candidate.device_address != query.device_address ||
                        candidate.port_numbers != query.port_numbers ||
                        candidate.serial_utf8 != query.serial_utf8 ||
                        candidate.interface_fingerprint !=
                            query.interface_fingerprint) {
                        device_error = make_error(
                            node_shape_is_valid(candidate)
                                ? WindowsUsbTopologyErrorKind::IdentityMismatch
                                : WindowsUsbTopologyErrorKind::MalformedSnapshot,
                            WindowsUsbTopologyStage::Correlation,
                            "Windows batch candidate does not match its libusb interface query",
                            candidate.device_instance_id_utf8,
                            WindowsUsbNativeErrorDomain::None,
                            0U,
                            group.session);
                        break;
                    }
                    auto physical_path = canonical_windows_usb_port_path(
                        candidate.bus_number, candidate.port_numbers);
                    if (!physical_path.has_value()) {
                        device_error = physical_path.error();
                        break;
                    }
                    resolved.push_back(WindowsUsbTopology{
                        .physical_port_path = std::move(*physical_path),
                        .root_controller_id =
                            "windows-pnp:" +
                            candidate.root_controller_instance_id_utf8,
                        .hub_port_chain = candidate.port_numbers,
                        .vendor_id = candidate.vendor_id,
                        .product_id = candidate.product_id,
                        .bus_number = candidate.bus_number,
                        .device_address = candidate.device_address,
                        .serial_utf8 = candidate.serial_utf8,
                        .interface_fingerprint =
                            candidate.interface_fingerprint,
                        .device_instance_id_utf8 =
                            candidate.device_instance_id_utf8,
                        .hub_instance_ids_utf8 =
                            candidate.hub_instance_ids_utf8,
                        .location_path_utf8 = candidate.location_path_utf8,
                    });
                }
            }

            if (device_error.has_value()) {
                for (const auto query_index : group.query_indices) {
                    staged[query_index].emplace(
                        std::unexpected(*device_error));
                }
            } else {
                for (std::size_t offset = 0U;
                     offset < group.query_indices.size();
                     ++offset) {
                    staged[group.query_indices[offset]].emplace(
                        std::move(resolved[offset]));
                }
            }
            if (const auto stop = interrupted(
                    WindowsUsbTopologyStage::Correlation,
                    deadline,
                    stop_token,
                    now_,
                    now_context_,
                    group.session);
                stop.has_value()) {
                return std::unexpected(*stop);
            }
        }

        std::vector<WindowsUsbTopologyResult> results;
        results.reserve(staged.size());
        for (auto& result : staged) {
            if (!result.has_value()) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::Correlation,
                    "Windows topology batch left an interface result unassigned"));
            }
            results.push_back(std::move(*result));
        }
        return results;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Correlation,
            "memory allocation failed while correlating a Windows USB topology batch",
            {},
            WindowsUsbNativeErrorDomain::None,
            0U,
            queries.empty() ? 0UL : queries.front().libusb_session_data));
    }
}

WindowsUsbTopologyQuery make_windows_usb_topology_query(
    const UsbDeviceInfo& device,
    const unsigned long libusb_session_data,
    const std::string_view device_instance_id_utf8) {
    return WindowsUsbTopologyQuery{
        .libusb_session_data = libusb_session_data,
        .device_instance_id_utf8 = std::string{device_instance_id_utf8},
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .port_numbers = device.port_path,
        .serial_utf8 = device.serial_utf8.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{device.serial_utf8},
        .interface_fingerprint = WindowsUsbInterfaceFingerprint{
            .interface_number = device.interface_number,
            .alternate_setting = device.alternate_setting,
            .interface_class = device.interface_class,
            .interface_subclass = device.interface_subclass,
            .interface_protocol = device.interface_protocol,
        },
    };
}

}  // namespace kairosboot::transport
