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
#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <windows.h>

#include <cfgmgr32.h>
#include <devpkey.h>
#include <setupapi.h>

#if defined(_MSC_VER)
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "SetupAPI.lib")
#endif
#endif

namespace kairosboot::transport {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 4U * 1024U;
#if defined(_WIN32)
constexpr std::size_t kMaximumPropertyBytes = 64U * 1024U;
constexpr std::size_t kMaximumParentDepth = 64U;
#endif

[[nodiscard]] WindowsUsbTopologyError make_error(
    const WindowsUsbTopologyErrorKind kind,
    const WindowsUsbTopologyStage stage,
    std::string message,
    std::string device_instance_id_utf8 = {},
    const WindowsUsbNativeErrorDomain native_domain =
        WindowsUsbNativeErrorDomain::None,
    const std::uint32_t native_code = 0U) {
    return WindowsUsbTopologyError{
        .kind = kind,
        .stage = stage,
        .native_domain = native_domain,
        .native_code = native_code,
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

[[nodiscard]] bool query_is_valid(
    const WindowsUsbTopologyQuery& query) noexcept {
    return query.vendor_id != 0U && query.product_id != 0U &&
        query.bus_number != 0U && query.device_address != 0U &&
        !query.port_numbers.empty() &&
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
    void* const now_context) {
    if (stop_token.stop_requested()) {
        return make_error(WindowsUsbTopologyErrorKind::Cancelled,
                          stage,
                          "Windows USB topology discovery was cancelled");
    }
    if (deadline != std::chrono::steady_clock::time_point::max() &&
        now(now_context) >= deadline) {
        return make_error(WindowsUsbTopologyErrorKind::TimedOut,
                          stage,
                          "Windows USB topology discovery deadline expired");
    }
    return std::nullopt;
}

[[nodiscard]] bool node_less(const WindowsUsbTopologyNode& left,
                             const WindowsUsbTopologyNode& right) {
    return std::tie(left.device_instance_id_utf8,
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
                    left.interface_fingerprint.interface_class,
                    left.interface_fingerprint.interface_subclass,
                    left.interface_fingerprint.interface_protocol) <
        std::tie(right.device_instance_id_utf8,
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
                 right.interface_fingerprint.interface_class,
                 right.interface_fingerprint.interface_subclass,
                 right.interface_fingerprint.interface_protocol);
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
    if (!valid_identity_text(node.device_instance_id_utf8) ||
        !valid_identity_text(node.root_controller_instance_id_utf8) ||
        !valid_identity_text(node.location_path_utf8) ||
        node.vendor_id == 0U || node.product_id == 0U ||
        node.bus_number == 0U || node.device_address == 0U ||
        node.port_numbers.empty() ||
        node.port_numbers.size() > kMaximumWindowsUsbTopologyDepth ||
        node.hub_instance_ids_utf8.empty() ||
        node.hub_instance_ids_utf8.size() >
            kMaximumWindowsUsbTopologyDepth + 1U ||
        std::ranges::any_of(node.port_numbers, [](const std::uint8_t port) {
            return port == 0U;
        }) ||
        (node.serial_utf8.has_value() &&
         !valid_identity_text(*node.serial_utf8)) ||
        std::ranges::any_of(node.hub_instance_ids_utf8,
                            [](const std::string& identity) {
                                return !valid_identity_text(identity);
                            }) ||
        !hub_chain_is_unique(node)) {
        return false;
    }
    const auto location =
        parse_windows_usb_location_path(node.location_path_utf8);
    return location.has_value() &&
        location->root_hub_index == node.root_hub_index &&
        location->port_numbers == node.port_numbers;
}

#if defined(_WIN32)

[[nodiscard]] std::optional<WindowsUsbTopologyError> native_interrupted(
    const WindowsUsbTopologyStage stage,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) {
    return interrupted(stage, deadline, stop_token, system_now, nullptr);
}

[[nodiscard]] WindowsUsbTopologyError win32_error(
    const WindowsUsbTopologyStage stage,
    const DWORD native_code,
    std::string message,
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
                      static_cast<std::uint32_t>(native_code));
}

[[nodiscard]] WindowsUsbTopologyError config_error(
    const WindowsUsbTopologyStage stage,
    const CONFIGRET native_code,
    std::string message,
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
                      static_cast<std::uint32_t>(native_code));
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
              const std::string& device_id) {
    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        ULONG required = 0U;
        auto result = CM_Get_DevNode_PropertyW(
            device, &key, &type, nullptr, &required, 0U);
        if (result == CR_NO_SUCH_VALUE) {
            return std::optional<DeviceProperty>{};
        }
        if (result == CR_NO_SUCH_DEVNODE) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device disappeared while a property was being sized",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result)));
        }
        if (result != CR_BUFFER_SMALL && result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage, result, "failed to size a device property", device_id));
        }
        if (required > kMaximumPropertyBytes) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                stage,
                "device property exceeds the bounded snapshot size",
                device_id));
        }

        DeviceProperty property{.type = type};
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
            return std::optional<DeviceProperty>{};
        }
        if (result == CR_NO_SUCH_DEVNODE) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device disappeared while a property was being read",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(result)));
        }
        if (result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage, result, "failed to read a device property", device_id));
        }
        if (actual > required) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                stage,
                "device property grew while it was being read",
                device_id,
                WindowsUsbNativeErrorDomain::ConfigurationManager,
                static_cast<std::uint32_t>(CR_BUFFER_SMALL)));
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
        static_cast<std::uint32_t>(CR_BUFFER_SMALL)));
}

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
wide_to_utf8(const std::wstring_view value,
             const WindowsUsbTopologyStage stage,
             const std::string& device_id) {
    if (value.empty() || value.size() > kMaximumIdentityBytes) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "Windows device identity text has an invalid size",
            device_id));
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "Windows device identity text is too large to convert",
            device_id));
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
            written == 0 ? static_cast<std::uint32_t>(GetLastError()) : 0U));
    }
    return converted;
}

[[nodiscard]] std::expected<std::string, WindowsUsbTopologyError>
device_instance_id(const DEVINST device,
                   const WindowsUsbTopologyStage stage) {
    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
        ULONG characters = 0U;
        auto result = CM_Get_Device_ID_Size(&characters, device, 0U);
        if (result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage, result, "failed to size a device instance identifier"));
        }
        if (characters == 0U || characters >= kMaximumIdentityBytes) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                stage,
                "device instance identifier has an invalid size"));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(characters) + 1U);
        result = CM_Get_Device_IDW(
            device, buffer.data(), static_cast<ULONG>(buffer.size()), 0U);
        if (result == CR_BUFFER_SMALL) {
            continue;
        }
        if (result != CR_SUCCESS) {
            return std::unexpected(config_error(
                stage, result, "failed to read a device instance identifier"));
        }
        const auto end = std::ranges::find(buffer, L'\0');
        if (end == buffer.end()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                stage,
                "device instance identifier is not terminated"));
        }
        return wide_to_utf8(
            std::wstring_view(buffer.data(),
                              static_cast<std::size_t>(end - buffer.begin())),
            stage,
            {});
    }
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::IdentityChanged,
        stage,
        "device instance identifier changed repeatedly while it was read",
        {},
        WindowsUsbNativeErrorDomain::ConfigurationManager,
        static_cast<std::uint32_t>(CR_BUFFER_SMALL)));
}

[[nodiscard]] std::expected<std::optional<std::int32_t>,
                            WindowsUsbTopologyError>
property_int32(const DEVINST device,
               const DEVPROPKEY& key,
               const WindowsUsbTopologyStage stage,
               const std::string& device_id) {
    auto property = read_property(device, key, stage, device_id);
    if (!property.has_value()) {
        return std::unexpected(property.error());
    }
    if (!property->has_value()) {
        return std::optional<std::int32_t>{};
    }
    if ((*property)->type != DEVPROP_TYPE_INT32 ||
        (*property)->bytes.size() != sizeof(std::int32_t)) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "device integer property has an unexpected type or size",
            device_id));
    }
    std::int32_t value = 0;
    std::memcpy(&value, (*property)->bytes.data(), sizeof(value));
    return std::optional<std::int32_t>{value};
}

[[nodiscard]] std::expected<std::optional<std::vector<std::string>>,
                            WindowsUsbTopologyError>
property_string_list(const DEVINST device,
                     const DEVPROPKEY& key,
                     const WindowsUsbTopologyStage stage,
                     const std::string& device_id) {
    auto property = read_property(device, key, stage, device_id);
    if (!property.has_value()) {
        return std::unexpected(property.error());
    }
    if (!property->has_value()) {
        return std::optional<std::vector<std::string>>{};
    }
    if ((*property)->type != DEVPROP_TYPE_STRING_LIST ||
        (*property)->bytes.size() < 2U * sizeof(wchar_t) ||
        (*property)->bytes.size() % sizeof(wchar_t) != 0U) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "device string-list property has an unexpected type or size",
            device_id));
    }
    std::vector<wchar_t> text((*property)->bytes.size() / sizeof(wchar_t));
    std::memcpy(text.data(), (*property)->bytes.data(), (*property)->bytes.size());
    if (text.size() < 2U || text[text.size() - 1U] != L'\0' ||
        text[text.size() - 2U] != L'\0') {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::MalformedSnapshot,
            stage,
            "device string-list property is not doubly terminated",
            device_id));
    }

    std::vector<std::string> values;
    std::size_t start = 0U;
    while (start + 1U < text.size()) {
        const auto end = std::ranges::find(text.begin() +
                                               static_cast<std::ptrdiff_t>(start),
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
                    "device string-list property has data after its terminator",
                    device_id));
            }
            break;
        }
        auto converted = wide_to_utf8(
            std::wstring_view(text.data() + start, length), stage, device_id);
        if (!converted.has_value()) {
            return std::unexpected(converted.error());
        }
        values.push_back(std::move(*converted));
        start += length + 1U;
    }
    return std::optional<std::vector<std::string>>{std::move(values)};
}

[[nodiscard]] std::optional<std::string> serial_from_instance_id(
    const std::string_view instance_id,
    const std::optional<std::string>& expected_serial) {
    if (!expected_serial.has_value()) {
        return std::nullopt;
    }
    const auto separator = instance_id.rfind('\\');
    if (separator == std::string_view::npos || separator + 1U == instance_id.size()) {
        return std::nullopt;
    }
    const auto suffix = instance_id.substr(separator + 1U);
    // Windows device instance IDs are case-insensitive. USB serial strings are
    // embedded in the instance component when the bus reports them as unique.
    if (!ascii_iequals(suffix, *expected_serial)) {
        return std::nullopt;
    }
    return *expected_serial;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_hex_marker(
    const std::string_view value,
    const std::string_view marker,
    const std::size_t digits) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (value.size() < marker.size() + digits) {
        return std::nullopt;
    }
    for (std::size_t offset = 0U;
         offset + marker.size() + digits <= value.size();
         ++offset) {
        if (!ascii_iequals(value.substr(offset, marker.size()), marker)) {
            continue;
        }
        unsigned int parsed = 0U;
        const auto token = value.substr(offset + marker.size(), digits);
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), parsed, 16);
        if (error == std::errc{} && end == token.data() + token.size() &&
            parsed <= static_cast<unsigned int>(
                std::numeric_limits<Integer>::max())) {
            return static_cast<Integer>(parsed);
        }
    }
    return std::nullopt;
}

struct HardwareIdentity final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::optional<std::uint8_t> interface_number;
};

[[nodiscard]] std::optional<HardwareIdentity> parse_hardware_identity(
    const std::string_view instance_id,
    const std::vector<std::string>& hardware_ids) {
    std::vector<std::string_view> values;
    values.reserve(hardware_ids.size() + 1U);
    values.push_back(instance_id);
    for (const auto& value : hardware_ids) {
        values.push_back(value);
    }
    for (const auto value : values) {
        const auto vendor =
            parse_hex_marker<std::uint16_t>(value, "VID_", 4U);
        const auto product =
            parse_hex_marker<std::uint16_t>(value, "PID_", 4U);
        if (vendor.has_value() && product.has_value()) {
            return HardwareIdentity{
                .vendor_id = *vendor,
                .product_id = *product,
                .interface_number =
                    parse_hex_marker<std::uint8_t>(value, "MI_", 2U),
            };
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<WindowsUsbInterfaceFingerprint>
parse_interface_fingerprint(const std::vector<std::string>& compatible_ids,
                            const std::uint8_t interface_number) {
    for (const auto& value : compatible_ids) {
        const auto interface_class =
            parse_hex_marker<std::uint8_t>(value, "CLASS_", 2U);
        const auto interface_subclass =
            parse_hex_marker<std::uint8_t>(value, "SUBCLASS_", 2U);
        const auto interface_protocol =
            parse_hex_marker<std::uint8_t>(value, "PROT_", 2U);
        if (interface_class.has_value() && interface_subclass.has_value() &&
            interface_protocol.has_value()) {
            return WindowsUsbInterfaceFingerprint{
                .interface_number = interface_number,
                .interface_class = *interface_class,
                .interface_subclass = *interface_subclass,
                .interface_protocol = *interface_protocol,
            };
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<std::optional<DEVINST>,
                            WindowsUsbTopologyError>
parent_of(const DEVINST device, const std::string& device_id) {
    DEVINST parent = 0U;
    const auto result = CM_Get_Parent(&parent, device, 0U);
    if (result == CR_NO_SUCH_DEVNODE) {
        return std::optional<DEVINST>{};
    }
    if (result != CR_SUCCESS) {
        return std::unexpected(config_error(
            WindowsUsbTopologyStage::ParentTraversal,
            result,
            "failed to read a USB devnode parent",
            device_id));
    }
    return std::optional<DEVINST>{parent};
}

[[nodiscard]] std::expected<std::optional<WindowsUsbLocationPath>,
                            WindowsUsbTopologyError>
unique_location_path(const DEVINST device, const std::string& device_id) {
    auto paths = property_string_list(device,
                                      DEVPKEY_Device_LocationPaths,
                                      WindowsUsbTopologyStage::PropertyRead,
                                      device_id);
    if (!paths.has_value()) {
        return std::unexpected(paths.error());
    }
    if (!paths->has_value() || (*paths)->empty()) {
        return std::optional<WindowsUsbLocationPath>{};
    }

    std::optional<WindowsUsbLocationPath> selected;
    for (const auto& path : **paths) {
        auto parsed = parse_windows_usb_location_path(path);
        if (!parsed.has_value()) {
            continue;
        }
        if (selected.has_value() && *selected != *parsed) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::AmbiguousMapping,
                WindowsUsbTopologyStage::PropertyRead,
                "device reports conflicting USB location paths",
                device_id));
        }
        selected = std::move(*parsed);
    }
    return selected;
}

struct TopologyAnchor final {
    DEVINST device{};
    std::string device_id;
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    WindowsUsbLocationPath location;
};

[[nodiscard]] std::expected<TopologyAnchor, WindowsUsbTopologyError>
find_topology_anchor(const DEVINST start,
                     const std::chrono::steady_clock::time_point deadline,
                     const std::stop_token stop_token) {
    DEVINST current = start;
    std::vector<std::string> visited;
    for (std::size_t depth = 0U; depth < kMaximumParentDepth; ++depth) {
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::ParentTraversal,
                deadline,
                stop_token);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        auto current_id = device_instance_id(
            current, WindowsUsbTopologyStage::ParentTraversal);
        if (!current_id.has_value()) {
            return std::unexpected(current_id.error());
        }
        if (std::ranges::find(visited, *current_id) != visited.end()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                WindowsUsbTopologyStage::ParentTraversal,
                "USB parent chain contains a cycle",
                *current_id));
        }
        visited.push_back(*current_id);

        auto bus = property_int32(current,
                                  DEVPKEY_Device_BusNumber,
                                  WindowsUsbTopologyStage::PropertyRead,
                                  *current_id);
        if (!bus.has_value()) {
            return std::unexpected(bus.error());
        }
        auto address = property_int32(current,
                                      DEVPKEY_Device_Address,
                                      WindowsUsbTopologyStage::PropertyRead,
                                      *current_id);
        if (!address.has_value()) {
            return std::unexpected(address.error());
        }
        auto location = unique_location_path(current, *current_id);
        if (!location.has_value()) {
            return std::unexpected(location.error());
        }
        if (bus->has_value() && address->has_value() && location->has_value()) {
            if (**bus <= 0 ||
                **bus > std::numeric_limits<std::uint8_t>::max() ||
                **address <= 0 ||
                **address > std::numeric_limits<std::uint8_t>::max()) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::PropertyRead,
                    "USB bus or address property is outside the libusb range",
                    *current_id));
            }
            return TopologyAnchor{
                .device = current,
                .device_id = std::move(*current_id),
                .bus_number = static_cast<std::uint8_t>(**bus),
                .device_address = static_cast<std::uint8_t>(**address),
                .location = std::move(**location),
            };
        }

        auto parent = parent_of(current, *current_id);
        if (!parent.has_value()) {
            return std::unexpected(parent.error());
        }
        if (!parent->has_value()) {
            break;
        }
        current = **parent;
    }
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::MalformedSnapshot,
        WindowsUsbTopologyStage::ParentTraversal,
        "USB devnode has no complete bus/address/location ancestor"));
}

struct ControllerChain final {
    std::string controller_id;
    std::vector<std::string> hub_ids;
};

[[nodiscard]] std::expected<bool, WindowsUsbTopologyError> is_usb_hub(
    const DEVINST device, const std::string& device_id) {
    if (ascii_istarts_with(device_id, "USB\\ROOT_HUB")) {
        return true;
    }
    auto compatible = property_string_list(device,
                                           DEVPKEY_Device_CompatibleIds,
                                           WindowsUsbTopologyStage::PropertyRead,
                                           device_id);
    if (!compatible.has_value()) {
        return std::unexpected(compatible.error());
    }
    if (!compatible->has_value()) {
        return false;
    }
    return std::ranges::any_of(**compatible, [](const std::string& value) {
        return parse_hex_marker<std::uint8_t>(value, "CLASS_", 2U) ==
            std::optional<std::uint8_t>{0x09U};
    });
}

[[nodiscard]] std::expected<ControllerChain, WindowsUsbTopologyError>
read_controller_chain(const DEVINST anchor,
                      const std::chrono::steady_clock::time_point deadline,
                      const std::stop_token stop_token) {
    DEVINST current = anchor;
    std::vector<std::string> visited;
    std::vector<std::string> hubs_leaf_to_root;
    for (std::size_t depth = 0U; depth < kMaximumParentDepth; ++depth) {
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::ParentTraversal,
                deadline,
                stop_token);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        auto current_id = device_instance_id(
            current, WindowsUsbTopologyStage::ParentTraversal);
        if (!current_id.has_value()) {
            return std::unexpected(current_id.error());
        }
        if (std::ranges::find(visited, *current_id) != visited.end()) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::MalformedSnapshot,
                WindowsUsbTopologyStage::ParentTraversal,
                "USB parent chain contains a cycle",
                *current_id));
        }
        visited.push_back(*current_id);

        const auto root_hub =
            ascii_istarts_with(*current_id, "USB\\ROOT_HUB");
        auto hub = is_usb_hub(current, *current_id);
        if (!hub.has_value()) {
            return std::unexpected(hub.error());
        }
        if (*hub) {
            hubs_leaf_to_root.push_back(*current_id);
            if (hubs_leaf_to_root.size() >
                kMaximumWindowsUsbTopologyDepth + 1U) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::TopologyTooDeep,
                    WindowsUsbTopologyStage::ParentTraversal,
                    "USB hub ancestry exceeds the supported depth",
                    *current_id));
            }
        }

        auto parent = parent_of(current, *current_id);
        if (!parent.has_value()) {
            return std::unexpected(parent.error());
        }
        if (!parent->has_value()) {
            break;
        }
        if (root_hub) {
            auto controller_id = device_instance_id(
                **parent, WindowsUsbTopologyStage::ParentTraversal);
            if (!controller_id.has_value()) {
                return std::unexpected(controller_id.error());
            }
            std::ranges::reverse(hubs_leaf_to_root);
            return ControllerChain{
                .controller_id = std::move(*controller_id),
                .hub_ids = std::move(hubs_leaf_to_root),
            };
        }
        current = **parent;
    }
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::MalformedSnapshot,
        WindowsUsbTopologyStage::ParentTraversal,
        "USB parent chain does not contain a root hub"));
}

struct NativeCandidate final {
    WindowsUsbTopologyNode node;
    bool explicit_interface{};
};

[[nodiscard]] std::expected<std::optional<NativeCandidate>,
                            WindowsUsbTopologyError>
candidate_from_devnode(const DEVINST device,
                       const WindowsUsbTopologyQuery& query,
                       const std::chrono::steady_clock::time_point deadline,
                       const std::stop_token stop_token) {
    auto id = device_instance_id(device, WindowsUsbTopologyStage::Enumeration);
    if (!id.has_value()) {
        return std::unexpected(id.error());
    }
    auto hardware_ids = property_string_list(device,
                                             DEVPKEY_Device_HardwareIds,
                                             WindowsUsbTopologyStage::PropertyRead,
                                             *id);
    if (!hardware_ids.has_value()) {
        return std::unexpected(hardware_ids.error());
    }
    const auto hardware = parse_hardware_identity(
        *id, hardware_ids->has_value() ? **hardware_ids
                                      : std::vector<std::string>{});
    if (!hardware.has_value() || hardware->vendor_id != query.vendor_id ||
        hardware->product_id != query.product_id ||
        (hardware->interface_number.has_value() &&
         *hardware->interface_number !=
             query.interface_fingerprint.interface_number) ||
        (!hardware->interface_number.has_value() &&
         query.interface_fingerprint.interface_number != 0U)) {
        return std::optional<NativeCandidate>{};
    }

    auto compatible_ids = property_string_list(device,
                                               DEVPKEY_Device_CompatibleIds,
                                               WindowsUsbTopologyStage::PropertyRead,
                                               *id);
    if (!compatible_ids.has_value()) {
        return std::unexpected(compatible_ids.error());
    }
    if (!compatible_ids->has_value()) {
        return std::optional<NativeCandidate>{};
    }
    const auto fingerprint = parse_interface_fingerprint(
        **compatible_ids, query.interface_fingerprint.interface_number);
    if (!fingerprint.has_value() ||
        *fingerprint != query.interface_fingerprint) {
        return std::optional<NativeCandidate>{};
    }

    auto anchor = find_topology_anchor(device, deadline, stop_token);
    if (!anchor.has_value()) {
        return std::unexpected(anchor.error());
    }
    auto chain = read_controller_chain(anchor->device, deadline, stop_token);
    if (!chain.has_value()) {
        return std::unexpected(chain.error());
    }

    const auto serial =
        serial_from_instance_id(anchor->device_id, query.serial_utf8);

    std::string location = anchor->location.controller_prefix_utf8;
    location += "#USBROOT(" +
        std::to_string(anchor->location.root_hub_index) + ")";
    for (const auto port : anchor->location.port_numbers) {
        location += "#USB(" + std::to_string(port) + ")";
    }
    return std::optional<NativeCandidate>{NativeCandidate{
        .node = WindowsUsbTopologyNode{
            .device_instance_id_utf8 = std::move(*id),
            .root_controller_instance_id_utf8 =
                std::move(chain->controller_id),
            .hub_instance_ids_utf8 = std::move(chain->hub_ids),
            .location_path_utf8 = std::move(location),
            .root_hub_index = anchor->location.root_hub_index,
            .vendor_id = hardware->vendor_id,
            .product_id = hardware->product_id,
            .bus_number = anchor->bus_number,
            .device_address = anchor->device_address,
            .port_numbers = std::move(anchor->location.port_numbers),
            .serial_utf8 = serial,
            .interface_fingerprint = *fingerprint,
        },
        .explicit_interface = hardware->interface_number.has_value(),
    }};
}

[[nodiscard]] bool same_physical_interface(const NativeCandidate& left,
                                            const NativeCandidate& right) {
    return left.node.vendor_id == right.node.vendor_id &&
        left.node.product_id == right.node.product_id &&
        left.node.bus_number == right.node.bus_number &&
        left.node.device_address == right.node.device_address &&
        left.node.port_numbers == right.node.port_numbers &&
        left.node.interface_fingerprint == right.node.interface_fingerprint;
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
        WindowsUsbLocationPath result;
        bool found_root = false;
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
                const auto index = parse_decimal<std::uint32_t>(component.substr(
                    root_prefix.size(),
                    component.size() - root_prefix.size() - 1U));
                if (!index.has_value() || result.controller_prefix_utf8.empty()) {
                    return std::unexpected(make_error(
                        WindowsUsbTopologyErrorKind::MalformedSnapshot,
                        WindowsUsbTopologyStage::Correlation,
                        "Windows USB root location component is malformed"));
                }
                result.root_hub_index = *index;
                found_root = true;
            } else if (found_root &&
                       ascii_istarts_with(component, port_prefix) &&
                       component.back() == ')') {
                const auto port = parse_decimal<std::uint8_t>(component.substr(
                    port_prefix.size(),
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
            } else if (found_root) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::Correlation,
                    "Windows USB location path has a non-USB component after its root"));
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

std::expected<std::vector<WindowsUsbTopologyNode>, WindowsUsbTopologyError>
SetupApiWindowsUsbTopologyBackend::read_candidates(
    const WindowsUsbTopologyQuery& query,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token stop_token) const {
#if !defined(_WIN32)
    (void)query;
    (void)deadline;
    (void)stop_token;
    return std::unexpected(make_error(
        WindowsUsbTopologyErrorKind::UnsupportedPlatform,
        WindowsUsbTopologyStage::Enumeration,
        "SetupAPI USB topology discovery is only available on Windows"));
#else
    try {
        if (!query_is_valid(query)) {
            return std::unexpected(make_error(
                query.port_numbers.size() > kMaximumWindowsUsbTopologyDepth
                    ? WindowsUsbTopologyErrorKind::TopologyTooDeep
                    : WindowsUsbTopologyErrorKind::InvalidArgument,
                WindowsUsbTopologyStage::Validation,
                "invalid libusb identity for Windows topology discovery"));
        }
        if (const auto stop = native_interrupted(
                WindowsUsbTopologyStage::Enumeration,
                deadline,
                stop_token);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        DeviceInfoSet devices(SetupDiGetClassDevsW(
            nullptr,
            L"USB",
            nullptr,
            DIGCF_PRESENT | DIGCF_ALLCLASSES));
        if (devices.get() == INVALID_HANDLE_VALUE) {
            return std::unexpected(win32_error(
                WindowsUsbTopologyStage::Enumeration,
                GetLastError(),
                "failed to enumerate present USB device nodes"));
        }

        std::vector<NativeCandidate> candidates;
        for (DWORD index = 0U;; ++index) {
            if (const auto stop = native_interrupted(
                    WindowsUsbTopologyStage::Enumeration,
                    deadline,
                    stop_token);
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
                    "present USB devnode enumeration failed"));
            }
            auto candidate = candidate_from_devnode(
                data.DevInst, query, deadline, stop_token);
            if (!candidate.has_value()) {
                return std::unexpected(candidate.error());
            }
            if (candidate->has_value()) {
                candidates.push_back(std::move(**candidate));
            }
        }

        // Composite devices may expose both a physical node and an MI_xx child.
        // Prefer the explicit MI node only for the same physical/interface key.
        std::vector<WindowsUsbTopologyNode> result;
        for (std::size_t index = 0U; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            const auto shadowed = !candidate.explicit_interface &&
                std::ranges::any_of(
                    candidates,
                    [&candidate](const NativeCandidate& other) {
                        return other.explicit_interface &&
                            same_physical_interface(candidate, other);
                    });
            if (!shadowed) {
                result.push_back(candidate.node);
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            WindowsUsbTopologyErrorKind::ResourceExhausted,
            WindowsUsbTopologyStage::Enumeration,
            "memory allocation failed during SetupAPI USB enumeration"));
    }
#endif
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
                "invalid libusb identity for Windows topology discovery"));
        }
        if (const auto stop = interrupted(WindowsUsbTopologyStage::Validation,
                                          deadline,
                                          stop_token,
                                          now_,
                                          now_context_);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        auto first = backend_.read_candidates(query, deadline, stop_token);
        if (!first.has_value()) {
            return std::unexpected(first.error());
        }
        if (const auto stop = interrupted(WindowsUsbTopologyStage::StabilityCheck,
                                          deadline,
                                          stop_token,
                                          now_,
                                          now_context_);
            stop.has_value()) {
            return std::unexpected(*stop);
        }
        auto second = backend_.read_candidates(query, deadline, stop_token);
        if (!second.has_value()) {
            return std::unexpected(second.error());
        }
        if (const auto stop = interrupted(WindowsUsbTopologyStage::StabilityCheck,
                                          deadline,
                                          stop_token,
                                          now_,
                                          now_context_);
            stop.has_value()) {
            return std::unexpected(*stop);
        }

        std::ranges::sort(*first, node_less);
        std::ranges::sort(*second, node_less);
        if (*first != *second) {
            return std::unexpected(make_error(
                WindowsUsbTopologyErrorKind::IdentityChanged,
                WindowsUsbTopologyStage::StabilityCheck,
                "Windows USB topology changed between validation snapshots"));
        }

        const WindowsUsbTopologyNode* match = nullptr;
        bool identity_mismatch = false;
        for (const auto& candidate : *second) {
            if (const auto stop = interrupted(WindowsUsbTopologyStage::Correlation,
                                              deadline,
                                              stop_token,
                                              now_,
                                              now_context_);
                stop.has_value()) {
                return std::unexpected(*stop);
            }
            if (!node_shape_is_valid(candidate)) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::MalformedSnapshot,
                    WindowsUsbTopologyStage::Correlation,
                    "SetupAPI backend returned a malformed USB topology node",
                    candidate.device_instance_id_utf8));
            }
            if (candidate.vendor_id != query.vendor_id ||
                candidate.product_id != query.product_id ||
                candidate.bus_number != query.bus_number ||
                candidate.device_address != query.device_address ||
                candidate.port_numbers != query.port_numbers ||
                candidate.interface_fingerprint != query.interface_fingerprint ||
                (query.serial_utf8.has_value() &&
                 (!candidate.serial_utf8.has_value() ||
                  *candidate.serial_utf8 != *query.serial_utf8))) {
                identity_mismatch = true;
                continue;
            }
            if (match != nullptr) {
                return std::unexpected(make_error(
                    WindowsUsbTopologyErrorKind::AmbiguousMapping,
                    WindowsUsbTopologyStage::Correlation,
                    "more than one Windows devnode maps to the libusb snapshot",
                    candidate.device_instance_id_utf8));
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
                    : "no present Windows devnode maps to the libusb snapshot"));
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
            "memory allocation failed while correlating Windows USB topology"));
    }
}

WindowsUsbTopologyQuery make_windows_usb_topology_query(
    const UsbDeviceInfo& device) {
    return WindowsUsbTopologyQuery{
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
            .interface_class = device.interface_class,
            .interface_subclass = device.interface_subclass,
            .interface_protocol = device.interface_protocol,
        },
    };
}

}  // namespace kairosboot::transport
