// SPDX-License-Identifier: MIT
#include "src/transport/macos_usb_topology.hpp"

#include "src/transport/libusb_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBHostFamilyDefinitions.h>
#endif

namespace kairosboot::transport {
namespace {

constexpr std::size_t kMaximumRegistryPathBytes = 4U * 1024U;
constexpr std::size_t kMaximumIdentityTextBytes = 256U;
constexpr std::size_t kMaximumRegistryCandidates = 4U * 1024U;
constexpr std::size_t kMaximumRegistryInterfaces = 4U * 1024U;
constexpr std::size_t kMaximumRegistryAncestry = 64U;

[[nodiscard]] MacUsbTopologyError make_error(
    const MacUsbTopologyErrorKind kind,
    const MacUsbTopologyStage stage,
    std::string message,
    std::string registry_path = {},
    const std::int32_t native_code = 0) {
    return MacUsbTopologyError{
        .kind = kind,
        .stage = stage,
        .native_code = native_code,
        .registry_path = std::move(registry_path),
        .message = std::move(message),
    };
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
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
    if (value.empty() || value.size() > kMaximumIdentityTextBytes ||
        !valid_utf8(value)) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte != 0x7FU;
    });
}

[[nodiscard]] bool valid_registry_path(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumRegistryPathBytes ||
        !valid_utf8(value)) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte != 0x7FU;
    });
}

[[nodiscard]] bool valid_optional_identity(
    const std::optional<std::string>& value) noexcept {
    return !value.has_value() || valid_identity_text(*value);
}

[[nodiscard]] bool query_is_valid(const MacUsbTopologyQuery& query) noexcept {
    return query.vendor_id != 0U && query.product_id != 0U &&
        query.device_address != 0U && query.session_id != 0U &&
        !query.port_numbers.empty() &&
        query.port_numbers.size() <= kMaximumMacUsbTopologyDepth &&
        std::ranges::all_of(query.port_numbers, [](const std::uint8_t port) {
            return port != 0U;
        }) &&
        valid_optional_identity(query.serial_utf8) &&
        valid_optional_identity(query.product_utf8);
}

[[nodiscard]] std::optional<MacUsbTopologyError> interruption_error(
    const MacUsbTopologyTimePoint deadline,
    const std::stop_token cancellation,
    const MacUsbTopologyStage stage) {
    if (cancellation.stop_requested()) {
        return make_error(MacUsbTopologyErrorKind::Cancelled,
                          stage,
                          "macOS USB topology discovery was cancelled");
    }
    if (MacUsbTopologyClock::now() >= deadline) {
        return make_error(MacUsbTopologyErrorKind::Timeout,
                          stage,
                          "macOS USB topology discovery exceeded its deadline");
    }
    return std::nullopt;
}

[[nodiscard]] bool interface_shape_is_valid(
    const MacUsbRegistryInterface& interface) noexcept {
    return interface.registry_entry_id != 0U &&
        valid_registry_path(interface.registry_path);
}

[[nodiscard]] bool node_shape_is_valid(const MacUsbRegistryNode& node) {
    if (node.registry_entry_id == 0U || node.session_id == 0U ||
        node.vendor_id == 0U || node.product_id == 0U ||
        node.device_address == 0U || node.port_numbers.empty() ||
        node.port_numbers.size() > kMaximumMacUsbTopologyDepth ||
        node.service_ancestry.empty() ||
        node.service_ancestry.size() > kMaximumRegistryAncestry ||
        node.interfaces.size() > kMaximumRegistryInterfaces ||
        !valid_optional_identity(node.serial_utf8) ||
        !valid_optional_identity(node.product_utf8) ||
        !valid_registry_path(node.registry_path) ||
        !std::ranges::all_of(node.port_numbers, [](const std::uint8_t port) {
            return port != 0U;
        }) ||
        !std::ranges::all_of(node.interfaces, interface_shape_is_valid)) {
        return false;
    }
    if (static_cast<std::uint8_t>(node.location_id >> 24U) !=
        node.bus_number) {
        return false;
    }
    bool saw_controller = false;
    for (std::size_t index = 0U; index < node.service_ancestry.size(); ++index) {
        const auto& ancestor = node.service_ancestry[index];
        if (ancestor.registry_entry_id == 0U ||
            ancestor.registry_entry_id == node.registry_entry_id ||
            !valid_registry_path(ancestor.registry_path)) {
            return false;
        }
        saw_controller = saw_controller ||
            ancestor.kind == MacUsbRegistryEntryKind::HostController ||
            ancestor.kind == MacUsbRegistryEntryKind::LegacyHostController;
        for (std::size_t other = index + 1U;
             other < node.service_ancestry.size();
             ++other) {
            if (ancestor.registry_entry_id ==
                node.service_ancestry[other].registry_entry_id) {
                return false;
            }
        }
    }
    if (!saw_controller) {
        return false;
    }
    for (std::size_t index = 0; index < node.interfaces.size(); ++index) {
        const auto& interface = node.interfaces[index];
        if (interface.registry_entry_id == node.registry_entry_id) {
            return false;
        }
        if (std::ranges::any_of(
                node.service_ancestry,
                [&interface](const MacUsbRegistryAncestor& ancestor) {
                    return ancestor.registry_entry_id ==
                        interface.registry_entry_id;
                })) {
            return false;
        }
        for (std::size_t other = index + 1U;
             other < node.interfaces.size();
             ++other) {
            if (interface.registry_entry_id ==
                node.interfaces[other].registry_entry_id) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] const MacUsbRegistryAncestor* root_controller(
    const MacUsbRegistryNode& node) noexcept {
    const auto match = std::ranges::find_if(
        node.service_ancestry, [](const MacUsbRegistryAncestor& ancestor) {
            return ancestor.kind == MacUsbRegistryEntryKind::HostController ||
                ancestor.kind ==
                    MacUsbRegistryEntryKind::LegacyHostController;
        });
    return match == node.service_ancestry.end() ? nullptr : &*match;
}

void normalize_snapshot(std::vector<MacUsbRegistryNode>& nodes) {
    for (auto& node : nodes) {
        std::ranges::sort(node.interfaces, {}, &MacUsbRegistryInterface::registry_entry_id);
    }
    std::ranges::sort(nodes, [](const auto& left, const auto& right) {
        return std::tie(left.registry_entry_id,
                        left.session_id,
                        left.location_id) <
            std::tie(right.registry_entry_id,
                     right.session_id,
                     right.location_id);
    });
}

[[nodiscard]] std::string controller_identifier(const std::uint64_t entry_id) {
    std::array<char, 16U> digits{};
    const auto [end, error] = std::to_chars(
        digits.data(), digits.data() + digits.size(), entry_id, 16);
    if (error != std::errc{}) {
        return {};
    }
    std::string result = "macos-iokit:";
    result.append(16U - static_cast<std::size_t>(end - digits.data()), '0');
    result.append(digits.data(), end);
    return result;
}

#if defined(__APPLE__)

[[nodiscard]] std::int32_t native_code(const kern_return_t code) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(code));
}

[[nodiscard]] MacUsbTopologyError native_error(
    const MacUsbTopologyStage stage,
    const kern_return_t code,
    std::string registry_path,
    std::string message) {
    MacUsbTopologyErrorKind kind = MacUsbTopologyErrorKind::IoError;
    if (code == kIOReturnNotPrivileged) {
        kind = MacUsbTopologyErrorKind::PermissionDenied;
    } else if (code == kIOReturnNoDevice || code == kIOReturnNotFound) {
        kind = MacUsbTopologyErrorKind::NotFound;
    } else if (code == kIOReturnNoMemory || code == kIOReturnNoResources) {
        kind = MacUsbTopologyErrorKind::ResourceExhausted;
    } else if (code == kIOReturnUnsupported) {
        kind = MacUsbTopologyErrorKind::UnsupportedPlatform;
    }
    return make_error(kind,
                      stage,
                      std::move(message),
                      std::move(registry_path),
                      native_code(code));
}

class UniqueIoObject final {
public:
    UniqueIoObject() = default;
    explicit UniqueIoObject(const io_object_t object) noexcept : object_(object) {}
    ~UniqueIoObject() {
        if (object_ != IO_OBJECT_NULL) {
            IOObjectRelease(object_);
        }
    }

    UniqueIoObject(const UniqueIoObject&) = delete;
    UniqueIoObject& operator=(const UniqueIoObject&) = delete;
    UniqueIoObject(UniqueIoObject&& other) noexcept
        : object_(std::exchange(other.object_, IO_OBJECT_NULL)) {}
    UniqueIoObject& operator=(UniqueIoObject&& other) noexcept {
        if (this != &other) {
            if (object_ != IO_OBJECT_NULL) {
                IOObjectRelease(object_);
            }
            object_ = std::exchange(other.object_, IO_OBJECT_NULL);
        }
        return *this;
    }

    [[nodiscard]] io_object_t get() const noexcept { return object_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return object_ != IO_OBJECT_NULL;
    }

private:
    io_object_t object_{IO_OBJECT_NULL};
};

class UniqueCfType final {
public:
    UniqueCfType() = default;
    explicit UniqueCfType(CFTypeRef value) noexcept : value_(value) {}
    ~UniqueCfType() {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
    }

    UniqueCfType(const UniqueCfType&) = delete;
    UniqueCfType& operator=(const UniqueCfType&) = delete;
    UniqueCfType(UniqueCfType&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    UniqueCfType& operator=(UniqueCfType&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                CFRelease(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] CFTypeRef get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    CFTypeRef value_{};
};

[[nodiscard]] UniqueCfType property(const io_registry_entry_t entry,
                                    const CFStringRef name) {
    return UniqueCfType(IORegistryEntryCreateCFProperty(
        entry, name, kCFAllocatorDefault, 0U));
}

[[nodiscard]] std::expected<std::optional<std::uint64_t>, MacUsbTopologyError>
read_unsigned_number(const io_registry_entry_t entry,
                     const CFStringRef name,
                     const std::string_view display_name,
                     const std::uint64_t maximum,
                     const MacUsbTopologyStage stage,
                     const std::string& registry_path) {
    const auto value = property(entry, name);
    if (!value) {
        return std::optional<std::uint64_t>{};
    }
    if (CFGetTypeID(value.get()) != CFNumberGetTypeID()) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry numeric property has the wrong CoreFoundation type",
            registry_path + ":" + std::string(display_name)));
    }
    std::int64_t number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()),
                          kCFNumberSInt64Type,
                          &number) ||
        number < 0 || static_cast<std::uint64_t>(number) > maximum) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry numeric property is outside its expected range",
            registry_path + ":" + std::string(display_name)));
    }
    return std::optional<std::uint64_t>{
        static_cast<std::uint64_t>(number)};
}

[[nodiscard]] std::expected<std::optional<std::uint32_t>, MacUsbTopologyError>
read_location_id(const io_registry_entry_t entry,
                 const MacUsbTopologyStage stage,
                 const std::string& registry_path) {
    const auto value = property(entry, CFSTR(kUSBHostPropertyLocationID));
    if (!value) {
        return std::optional<std::uint32_t>{};
    }
    if (CFGetTypeID(value.get()) != CFNumberGetTypeID()) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry locationID has the wrong CoreFoundation type",
            registry_path + ":" + kUSBHostPropertyLocationID));
    }
    std::int32_t signed_location = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()),
                          kCFNumberSInt32Type,
                          &signed_location)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry locationID is not a 32-bit value",
            registry_path + ":" + kUSBHostPropertyLocationID));
    }
    return std::optional<std::uint32_t>{
        std::bit_cast<std::uint32_t>(signed_location)};
}

[[nodiscard]] std::expected<std::uint64_t, MacUsbTopologyError>
required_unsigned_number(const io_registry_entry_t entry,
                         const CFStringRef name,
                         const std::string_view display_name,
                         const std::uint64_t maximum,
                         const MacUsbTopologyStage stage,
                         const std::string& registry_path) {
    const auto value = read_unsigned_number(
        entry, name, display_name, maximum, stage, registry_path);
    if (!value.has_value()) {
        return std::unexpected(value.error());
    }
    if (!value->has_value()) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "required IORegistry numeric property is missing",
            registry_path + ":" + std::string(display_name)));
    }
    return **value;
}

[[nodiscard]] std::expected<std::string, MacUsbTopologyError>
registry_path(const io_registry_entry_t entry,
              const io_name_t plane,
              const MacUsbTopologyStage stage) {
    io_string_t buffer{};
    const auto result = IORegistryEntryGetPath(entry, plane, buffer);
    if (result != kIOReturnSuccess) {
        return std::unexpected(native_error(
            stage, result, {}, "failed to read an IORegistry entry path"));
    }
    std::string path(buffer);
    if (!valid_registry_path(path)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry returned an invalid or overlong entry path",
            std::move(path)));
    }
    return path;
}

[[nodiscard]] std::expected<std::uint64_t, MacUsbTopologyError>
registry_entry_id(const io_registry_entry_t entry,
                  const MacUsbTopologyStage stage,
                  const std::string& registry_path_value) {
    std::uint64_t entry_id = 0;
    const auto result = IORegistryEntryGetRegistryEntryID(entry, &entry_id);
    if (result != kIOReturnSuccess) {
        return std::unexpected(native_error(
            stage,
            result,
            registry_path_value,
            "failed to read an IORegistry entry ID"));
    }
    if (entry_id == 0U) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry returned a zero entry ID",
            registry_path_value));
    }
    return entry_id;
}

[[nodiscard]] std::expected<std::optional<std::string>, MacUsbTopologyError>
read_text_value(const io_registry_entry_t entry,
                const CFStringRef name,
                const std::string_view display_name,
                const MacUsbTopologyStage stage,
                const std::string& registry_path_value) {
    const auto value = property(entry, name);
    if (!value) {
        return std::optional<std::string>{};
    }

    std::string text;
    if (CFGetTypeID(value.get()) == CFStringGetTypeID()) {
        const auto string = static_cast<CFStringRef>(value.get());
        const auto length = CFStringGetLength(string);
        const auto maximum = CFStringGetMaximumSizeForEncoding(
            length, kCFStringEncodingUTF8);
        if (maximum < 0 ||
            maximum > static_cast<CFIndex>(kMaximumIdentityTextBytes)) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                stage,
                "IORegistry identity text exceeds the bounded UTF-8 limit",
                registry_path_value + ":" + std::string(display_name)));
        }
        std::vector<char> buffer(static_cast<std::size_t>(maximum) + 1U);
        if (!CFStringGetCString(string,
                               buffer.data(),
                               static_cast<CFIndex>(buffer.size()),
                               kCFStringEncodingUTF8)) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                stage,
                "IORegistry identity text is not valid UTF-8",
                registry_path_value + ":" + std::string(display_name)));
        }
        text.assign(buffer.data());
    } else if (CFGetTypeID(value.get()) == CFDataGetTypeID()) {
        const auto data = static_cast<CFDataRef>(value.get());
        const auto length = CFDataGetLength(data);
        if (length < 0 ||
            length > static_cast<CFIndex>(kMaximumIdentityTextBytes)) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                stage,
                "IORegistry identity data exceeds the bounded UTF-8 limit",
                registry_path_value + ":" + std::string(display_name)));
        }
        if (length == 0) {
            return std::optional<std::string>{};
        }
        const auto* bytes = CFDataGetBytePtr(data);
        if (bytes == nullptr) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                stage,
                "IORegistry identity data has no readable bytes",
                registry_path_value + ":" + std::string(display_name)));
        }
        text.assign(reinterpret_cast<const char*>(bytes),
                    static_cast<std::size_t>(length));
    } else {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry identity property has the wrong CoreFoundation type",
            registry_path_value + ":" + std::string(display_name)));
    }
    if (text.empty()) {
        return std::optional<std::string>{};
    }
    if (!valid_identity_text(text)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry identity property is not bounded printable UTF-8",
            registry_path_value + ":" + std::string(display_name)));
    }
    return std::optional<std::string>{std::move(text)};
}

[[nodiscard]] std::expected<std::optional<std::string>, MacUsbTopologyError>
read_compatible_text(const io_registry_entry_t entry,
                     const std::array<std::pair<CFStringRef, std::string_view>, 2U>&
                         keys,
                     const MacUsbTopologyStage stage,
                     const std::string& registry_path_value) {
    std::optional<std::string> result;
    for (const auto& [key, display_name] : keys) {
        const auto value = read_text_value(
            entry, key, display_name, stage, registry_path_value);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        if (!value->has_value()) {
            continue;
        }
        if (result.has_value() && *result != **value) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                stage,
                "compatible IORegistry identity properties disagree",
                registry_path_value));
        }
        result = std::move(**value);
    }
    return result;
}

[[nodiscard]] std::expected<std::optional<std::uint64_t>, MacUsbTopologyError>
read_session_id(const io_registry_entry_t entry,
                const MacUsbTopologyStage stage,
                const std::string& path) {
    const auto value = property(entry, CFSTR("sessionID"));
    if (!value) {
        return std::optional<std::uint64_t>{};
    }
    if (CFGetTypeID(value.get()) != CFNumberGetTypeID()) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry sessionID has the wrong CoreFoundation type",
            path + ":sessionID"));
    }
    std::int64_t signed_session = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()),
                          kCFNumberSInt64Type,
                          &signed_session)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            stage,
            "IORegistry sessionID is not a 64-bit value",
            path + ":sessionID"));
    }
    return std::optional<std::uint64_t>{
        std::bit_cast<std::uint64_t>(signed_session)};
}

// Enumeration can contain unrelated, malformed registry entries. Do not read
// their path or any other property: only an exact numeric session match is
// allowed to enter the strict query-scoped snapshot path below.
[[nodiscard]] std::optional<std::uint64_t> session_id_filter(
    const io_registry_entry_t entry) noexcept {
    const auto value = property(entry, CFSTR("sessionID"));
    if (!value || CFGetTypeID(value.get()) != CFNumberGetTypeID()) {
        return std::nullopt;
    }
    std::int64_t signed_session = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()),
                          kCFNumberSInt64Type,
                          &signed_session)) {
        return std::nullopt;
    }
    return std::bit_cast<std::uint64_t>(signed_session);
}

[[nodiscard]] std::expected<std::optional<std::uint8_t>, MacUsbTopologyError>
read_port_number_property(const io_registry_entry_t entry,
                          const std::string& path) {
    const auto value = property(entry, CFSTR("PortNum"));
    if (!value) {
        return std::optional<std::uint8_t>{};
    }
    if (CFGetTypeID(value.get()) != CFNumberGetTypeID()) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IORegistry PortNum has the wrong CoreFoundation type",
            path + ":PortNum"));
    }
    std::int8_t signed_port = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value.get()),
                          kCFNumberSInt8Type,
                          &signed_port)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IORegistry PortNum is not an 8-bit value",
            path + ":PortNum"));
    }
    return std::optional<std::uint8_t>{
        std::bit_cast<std::uint8_t>(signed_port)};
}

[[nodiscard]] std::expected<std::optional<std::uint8_t>, MacUsbTopologyError>
read_port_data_property(const io_registry_entry_t entry,
                        const CFStringRef key,
                        const std::string_view display_name,
                        const std::string& path) {
    const auto value = property(entry, key);
    if (!value) {
        return std::optional<std::uint8_t>{};
    }
    if (CFGetTypeID(value.get()) != CFDataGetTypeID()) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IORegistry port-service property has the wrong CoreFoundation type",
            path + ":" + std::string(display_name)));
    }
    const auto data = static_cast<CFDataRef>(value.get());
    if (CFDataGetLength(data) < 1 || CFDataGetBytePtr(data) == nullptr) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IORegistry port-service property has no port byte",
            path + ":" + std::string(display_name)));
    }
    return std::optional<std::uint8_t>{CFDataGetBytePtr(data)[0]};
}

[[nodiscard]] std::expected<std::optional<std::uint8_t>, MacUsbTopologyError>
read_device_port(const io_registry_entry_t device,
                 const std::string& device_path) {
    auto direct = read_port_number_property(device, device_path);
    if (!direct.has_value() || direct->has_value()) {
        return direct;
    }

    io_registry_entry_t raw_parent = IO_OBJECT_NULL;
    const auto parent_result = IORegistryEntryGetParentEntry(
        device, kIOServicePlane, &raw_parent);
    if (parent_result == kIOReturnNoDevice ||
        parent_result == kIOReturnNotFound) {
        return std::optional<std::uint8_t>{};
    }
    if (parent_result != kIOReturnSuccess || raw_parent == IO_OBJECT_NULL) {
        return std::unexpected(native_error(
            MacUsbTopologyStage::Hierarchy,
            parent_result,
            device_path,
            "failed to locate the device's USB port service"));
    }
    const UniqueIoObject parent(raw_parent);
    auto parent_path = registry_path(
        parent.get(), kIOServicePlane, MacUsbTopologyStage::Hierarchy);
    if (!parent_path.has_value()) {
        return std::unexpected(parent_path.error());
    }
    auto legacy = read_port_data_property(
        parent.get(), CFSTR("port"), "port", *parent_path);
    if (!legacy.has_value()) {
        return std::unexpected(legacy.error());
    }
    auto modern = read_port_data_property(
        parent.get(), CFSTR("usb-port-number"), "usb-port-number", *parent_path);
    if (!modern.has_value()) {
        return std::unexpected(modern.error());
    }
    if (legacy->has_value() && modern->has_value() && **legacy != **modern) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IORegistry port-service properties disagree",
            *parent_path));
    }
    return legacy->has_value() ? *legacy : *modern;
}

struct ParentSessionEntry final {
    UniqueIoObject service;
    std::uint64_t session_id{};
};

[[nodiscard]] std::expected<std::optional<ParentSessionEntry>,
                            MacUsbTopologyError>
nearest_parent_session(const io_registry_entry_t child,
                       const MacUsbTopologyTimePoint deadline,
                       const std::stop_token cancellation) {
    UniqueIoObject owned_current;
    io_registry_entry_t current = child;
    for (std::size_t depth = 0U; depth < kMaximumRegistryAncestry; ++depth) {
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::Hierarchy)) {
            return std::unexpected(*interrupted);
        }
        io_registry_entry_t raw_parent = IO_OBJECT_NULL;
        const auto parent_result = IORegistryEntryGetParentEntry(
            current, kIOUSBPlane, &raw_parent);
        if (parent_result == kIOReturnNoDevice ||
            parent_result == kIOReturnNotFound) {
            return std::optional<ParentSessionEntry>{};
        }
        if (parent_result != kIOReturnSuccess || raw_parent == IO_OBJECT_NULL) {
            return std::unexpected(native_error(
                MacUsbTopologyStage::Hierarchy,
                parent_result,
                {},
                "failed to traverse the IOUSB parent-session chain"));
        }
        UniqueIoObject parent(raw_parent);
        auto path = registry_path(
            parent.get(), kIOUSBPlane, MacUsbTopologyStage::Hierarchy);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        auto session = read_session_id(
            parent.get(), MacUsbTopologyStage::Hierarchy, *path);
        if (!session.has_value()) {
            return std::unexpected(session.error());
        }
        if (session->has_value()) {
            if (**session == 0U) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Hierarchy,
                    "IOUSB parent has a zero sessionID",
                    *path));
            }
            return std::optional<ParentSessionEntry>{ParentSessionEntry{
                .service = std::move(parent),
                .session_id = **session,
            }};
        }
        owned_current = std::move(parent);
        current = owned_current.get();
    }
    return std::unexpected(make_error(
        MacUsbTopologyErrorKind::TopologyTooDeep,
        MacUsbTopologyStage::Hierarchy,
        "IOUSB parent-session traversal exceeds the bounded registry depth"));
}

[[nodiscard]] MacUsbRegistryEntryKind registry_entry_kind(
    const io_registry_entry_t entry) noexcept {
    if (IOObjectConformsTo(entry, "IOUSBHostController")) {
        return MacUsbRegistryEntryKind::HostController;
    }
    if (IOObjectConformsTo(entry, "IOUSBController")) {
        return MacUsbRegistryEntryKind::LegacyHostController;
    }
    if (IOObjectConformsTo(entry, "IOUSBRootHubDevice")) {
        return MacUsbRegistryEntryKind::UsbRootHub;
    }
    if (IOObjectConformsTo(entry, kIOUSBHostDeviceClassName) ||
        IOObjectConformsTo(entry, "IOUSBDevice")) {
        return MacUsbRegistryEntryKind::UsbDevice;
    }
    return MacUsbRegistryEntryKind::Other;
}

[[nodiscard]] std::expected<std::vector<MacUsbRegistryAncestor>,
                            MacUsbTopologyError>
read_service_ancestry(const io_registry_entry_t device,
                      const MacUsbTopologyTimePoint deadline,
                      const std::stop_token cancellation) {
    std::vector<MacUsbRegistryAncestor> ancestry;
    UniqueIoObject owned_current;
    io_registry_entry_t current = device;
    for (std::size_t depth = 0U; depth < kMaximumRegistryAncestry; ++depth) {
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::Hierarchy)) {
            return std::unexpected(*interrupted);
        }
        io_registry_entry_t raw_parent = IO_OBJECT_NULL;
        const auto parent_result = IORegistryEntryGetParentEntry(
            current, kIOServicePlane, &raw_parent);
        if (parent_result == kIOReturnNoDevice ||
            parent_result == kIOReturnNotFound) {
            break;
        }
        if (parent_result != kIOReturnSuccess || raw_parent == IO_OBJECT_NULL) {
            return std::unexpected(native_error(
                MacUsbTopologyStage::Hierarchy,
                parent_result,
                {},
                "failed to traverse the IOService controller ancestry"));
        }
        owned_current = UniqueIoObject(raw_parent);
        current = owned_current.get();
        auto path = registry_path(
            current, kIOServicePlane, MacUsbTopologyStage::Hierarchy);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        auto entry_id = registry_entry_id(
            current, MacUsbTopologyStage::Hierarchy, *path);
        if (!entry_id.has_value()) {
            return std::unexpected(entry_id.error());
        }
        const auto kind = registry_entry_kind(current);
        ancestry.push_back(MacUsbRegistryAncestor{
            .registry_entry_id = *entry_id,
            .kind = kind,
            .registry_path = std::move(*path),
        });
        if (kind == MacUsbRegistryEntryKind::HostController ||
            kind == MacUsbRegistryEntryKind::LegacyHostController) {
            if (const auto interrupted = interruption_error(
                    deadline, cancellation, MacUsbTopologyStage::Hierarchy)) {
                return std::unexpected(*interrupted);
            }
            return ancestry;
        }
    }
    return std::unexpected(make_error(
        ancestry.size() == kMaximumRegistryAncestry
            ? MacUsbTopologyErrorKind::TopologyTooDeep
            : MacUsbTopologyErrorKind::MalformedRegistry,
        MacUsbTopologyStage::Hierarchy,
        ancestry.size() == kMaximumRegistryAncestry
            ? "IOService controller ancestry exceeds the bounded registry depth"
            : "IOService ancestry contains no recognized USB host controller"));
}

struct HierarchyResult final {
    std::vector<std::uint8_t> port_numbers;
    std::vector<MacUsbRegistryAncestor> service_ancestry;
};

[[nodiscard]] std::expected<HierarchyResult, MacUsbTopologyError>
read_hierarchy(const io_registry_entry_t device,
               const std::uint64_t leaf_session_id,
               const MacUsbTopologyTimePoint deadline,
               const std::stop_token cancellation) {
    std::vector<std::uint8_t> reverse_ports;
    std::vector<std::uint64_t> sessions{leaf_session_id};
    UniqueIoObject owned_current;
    io_registry_entry_t current = device;
    std::uint64_t current_session_id = leaf_session_id;

    while (true) {
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::Hierarchy)) {
            return std::unexpected(*interrupted);
        }
        auto path = registry_path(
            current, kIOUSBPlane, MacUsbTopologyStage::Hierarchy);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        auto observed_session = read_session_id(
            current, MacUsbTopologyStage::Hierarchy, *path);
        if (!observed_session.has_value()) {
            return std::unexpected(observed_session.error());
        }
        if (!observed_session->has_value() ||
            **observed_session != current_session_id) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::Hierarchy,
                "IOUSB session identity changed while its port chain was read",
                *path));
        }
        auto port = read_device_port(current, *path);
        if (!port.has_value()) {
            return std::unexpected(port.error());
        }
        if (!port->has_value()) {
            if (reverse_ports.empty()) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Hierarchy,
                    "leaf IOUSB device has no readable port number",
                    *path));
            }
            break;
        }
        if (**port == 0U) {
            if (reverse_ports.empty()) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Hierarchy,
                    "leaf IOUSB device has a zero port number",
                    *path));
            }
            break;
        }
        if (reverse_ports.size() == kMaximumMacUsbTopologyDepth) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::TopologyTooDeep,
                MacUsbTopologyStage::Hierarchy,
                "IOUSB parent-session chain exceeds the USB tier limit",
                *path));
        }
        reverse_ports.push_back(**port);

        auto parent = nearest_parent_session(current, deadline, cancellation);
        if (!parent.has_value()) {
            return std::unexpected(parent.error());
        }
        if (!parent->has_value()) {
            break;
        }
        if (std::ranges::find(sessions, (**parent).session_id) !=
            sessions.end()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::Hierarchy,
                "IOUSB parent-session chain contains a cycle",
                *path));
        }
        sessions.push_back((**parent).session_id);
        current_session_id = (**parent).session_id;
        owned_current = std::move((**parent).service);
        current = owned_current.get();
    }

    std::ranges::reverse(reverse_ports);
    auto ancestry = read_service_ancestry(device, deadline, cancellation);
    if (!ancestry.has_value()) {
        return std::unexpected(ancestry.error());
    }
    if (const auto interrupted = interruption_error(
            deadline, cancellation, MacUsbTopologyStage::Hierarchy)) {
        return std::unexpected(*interrupted);
    }
    return HierarchyResult{
        .port_numbers = std::move(reverse_ports),
        .service_ancestry = std::move(*ancestry),
    };
}

[[nodiscard]] std::expected<std::optional<std::uint64_t>, MacUsbTopologyError>
nearest_usb_device_id(const io_registry_entry_t interface,
                      const MacUsbTopologyTimePoint deadline,
                      const std::stop_token cancellation,
                      const std::string& interface_path) {
    UniqueIoObject owned_current;
    io_registry_entry_t current = interface;
    for (std::size_t depth = 0U; depth < kMaximumRegistryAncestry; ++depth) {
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::InterfaceEnumeration)) {
            return std::unexpected(*interrupted);
        }
        io_registry_entry_t parent = IO_OBJECT_NULL;
        const auto parent_result = IORegistryEntryGetParentEntry(
            current, kIOServicePlane, &parent);
        if (parent_result == kIOReturnNoDevice ||
            parent_result == kIOReturnNotFound) {
            return std::optional<std::uint64_t>{};
        }
        if (parent_result != kIOReturnSuccess || parent == IO_OBJECT_NULL) {
            return std::unexpected(native_error(
                MacUsbTopologyStage::InterfaceEnumeration,
                parent_result,
                interface_path,
                "failed to locate an interface's owning USB device"));
        }
        owned_current = UniqueIoObject(parent);
        current = owned_current.get();
        if (IOObjectConformsTo(current, kIOUSBHostDeviceClassName) ||
            IOObjectConformsTo(current, "IOUSBDevice")) {
            auto path = registry_path(current,
                                      kIOServicePlane,
                                      MacUsbTopologyStage::InterfaceEnumeration);
            if (!path.has_value()) {
                return std::unexpected(path.error());
            }
            auto entry_id = registry_entry_id(
                current, MacUsbTopologyStage::InterfaceEnumeration, *path);
            if (!entry_id.has_value()) {
                return std::unexpected(entry_id.error());
            }
            return std::optional<std::uint64_t>{*entry_id};
        }
    }
    return std::unexpected(make_error(
        MacUsbTopologyErrorKind::TopologyTooDeep,
        MacUsbTopologyStage::InterfaceEnumeration,
        "IOService interface ancestry exceeds the bounded traversal depth",
        interface_path));
}

[[nodiscard]] std::expected<std::vector<MacUsbRegistryInterface>,
                            MacUsbTopologyError>
read_interfaces(const io_registry_entry_t device,
                const std::uint64_t device_entry_id,
                const std::string& device_path,
                const MacUsbTopologyTimePoint deadline,
                const std::stop_token cancellation) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto iterator_result = IORegistryEntryCreateIterator(
        device,
        kIOServicePlane,
        kIORegistryIterateRecursively,
        &iterator);
    if (iterator_result != kIOReturnSuccess) {
        return std::unexpected(native_error(
            MacUsbTopologyStage::InterfaceEnumeration,
            iterator_result,
            device_path,
            "failed to enumerate USB interfaces below an IORegistry device"));
    }
    if (iterator == IO_OBJECT_NULL) {
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::InterfaceEnumeration)) {
            return std::unexpected(*interrupted);
        }
        return std::vector<MacUsbRegistryInterface>{};
    }
    UniqueIoObject owned_iterator(iterator);
    std::vector<MacUsbRegistryInterface> interfaces;
    while (true) {
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::InterfaceEnumeration)) {
            return std::unexpected(*interrupted);
        }
        UniqueIoObject service(IOIteratorNext(iterator));
        if (!service) {
            break;
        }
        if (!IOObjectConformsTo(service.get(), kIOUSBHostInterfaceClassName) &&
            !IOObjectConformsTo(service.get(), "IOUSBInterface")) {
            continue;
        }
        auto path = registry_path(service.get(),
                                  kIOServicePlane,
                                  MacUsbTopologyStage::InterfaceEnumeration);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        auto owner = nearest_usb_device_id(
            service.get(), deadline, cancellation, *path);
        if (!owner.has_value()) {
            return std::unexpected(owner.error());
        }
        if (!owner->has_value() || **owner != device_entry_id) {
            continue;
        }
        if (interfaces.size() == kMaximumRegistryInterfaces) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::InterfaceEnumeration,
                "USB interface enumeration exceeds the bounded candidate limit",
                device_path));
        }
        auto interface_entry_id = registry_entry_id(
            service.get(), MacUsbTopologyStage::InterfaceEnumeration, *path);
        if (!interface_entry_id.has_value()) {
            return std::unexpected(interface_entry_id.error());
        }
        const auto read_byte = [&](const CFStringRef key,
                                   const std::string_view name)
            -> std::expected<std::uint8_t, MacUsbTopologyError> {
            auto value = required_unsigned_number(
                service.get(),
                key,
                name,
                std::numeric_limits<std::uint8_t>::max(),
                MacUsbTopologyStage::InterfaceEnumeration,
                *path);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            return static_cast<std::uint8_t>(*value);
        };
        const auto configuration = read_byte(
            CFSTR(kUSBHostMatchingPropertyConfigurationValue),
            kUSBHostMatchingPropertyConfigurationValue);
        const auto number = read_byte(
            CFSTR(kUSBHostMatchingPropertyInterfaceNumber),
            kUSBHostMatchingPropertyInterfaceNumber);
        const auto alternate = read_byte(
            CFSTR(kUSBHostInterfacePropertyAlternateSetting),
            kUSBHostInterfacePropertyAlternateSetting);
        const auto interface_class = read_byte(
            CFSTR(kUSBHostMatchingPropertyInterfaceClass),
            kUSBHostMatchingPropertyInterfaceClass);
        const auto subclass = read_byte(
            CFSTR(kUSBHostMatchingPropertyInterfaceSubClass),
            kUSBHostMatchingPropertyInterfaceSubClass);
        const auto protocol = read_byte(
            CFSTR(kUSBHostMatchingPropertyInterfaceProtocol),
            kUSBHostMatchingPropertyInterfaceProtocol);
        if (!configuration.has_value()) {
            return std::unexpected(configuration.error());
        }
        if (!number.has_value()) {
            return std::unexpected(number.error());
        }
        if (!alternate.has_value()) {
            return std::unexpected(alternate.error());
        }
        if (!interface_class.has_value()) {
            return std::unexpected(interface_class.error());
        }
        if (!subclass.has_value()) {
            return std::unexpected(subclass.error());
        }
        if (!protocol.has_value()) {
            return std::unexpected(protocol.error());
        }
        interfaces.push_back(MacUsbRegistryInterface{
            .registry_entry_id = *interface_entry_id,
            .fingerprint = MacUsbInterfaceFingerprint{
                .configuration_value = *configuration,
                .interface_number = *number,
                .alternate_setting = *alternate,
                .interface_class = *interface_class,
                .interface_subclass = *subclass,
                .interface_protocol = *protocol,
            },
            .registry_path = std::move(*path),
        });
    }
    if (!IOIteratorIsValid(iterator)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::IdentityChanged,
            MacUsbTopologyStage::InterfaceEnumeration,
            "IORegistry interface iterator was invalidated during enumeration",
            device_path));
    }
    if (const auto interrupted = interruption_error(
            deadline,
            cancellation,
            MacUsbTopologyStage::InterfaceEnumeration)) {
        return std::unexpected(*interrupted);
    }
    std::ranges::sort(interfaces, {}, &MacUsbRegistryInterface::registry_entry_id);
    return interfaces;
}

[[nodiscard]] std::expected<std::vector<MacUsbRegistryNode>,
                            MacUsbTopologyError>
iokit_snapshot(const MacUsbTopologyQuery& query,
               const MacUsbTopologyTimePoint deadline,
               const std::stop_token cancellation) {
    if (const auto interrupted = interruption_error(
            deadline,
            cancellation,
            MacUsbTopologyStage::DeviceEnumeration)) {
        return std::unexpected(*interrupted);
    }
    // Match the same base class as frozen libusb 1.0.30. This includes modern
    // IOUSBHostDevice services and legacy Intel IOUSBDevice subclasses.
    auto* matching = IOServiceMatching("IOUSBDevice");
    if (matching == nullptr) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::ResourceExhausted,
            MacUsbTopologyStage::DeviceEnumeration,
            "IOKit could not allocate a USB device matching dictionary"));
    }
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto iterator_result = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iterator);
    if (iterator_result != kIOReturnSuccess) {
        return std::unexpected(native_error(
            MacUsbTopologyStage::DeviceEnumeration,
            iterator_result,
            {},
            "failed to enumerate IOUSBDevice services"));
    }
    // IOKit may report a successful empty enumeration with a null iterator.
    // Treat it exactly like an exhausted iterator, not as a native failure.
    if (iterator == IO_OBJECT_NULL) {
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }
        return std::vector<MacUsbRegistryNode>{};
    }
    UniqueIoObject owned_iterator(iterator);
    std::vector<MacUsbRegistryNode> nodes;

    while (true) {
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::DeviceEnumeration)) {
            return std::unexpected(*interrupted);
        }
        UniqueIoObject service(IOIteratorNext(iterator));
        if (!service) {
            break;
        }
        const auto filtered_session = session_id_filter(service.get());
        if (!filtered_session.has_value() ||
            *filtered_session != query.session_id) {
            continue;
        }
        auto path = registry_path(service.get(),
                                  kIOUSBPlane,
                                  MacUsbTopologyStage::DeviceSnapshot);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        // Session is the only Darwin identity shared losslessly with libusb.
        // Read and match it before consulting location, descriptors, address,
        // interfaces, or controller hierarchy.
        const auto session = read_session_id(
            service.get(), MacUsbTopologyStage::DeviceSnapshot, *path);
        if (!session.has_value()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::DeviceSnapshot,
                "session-matched IOUSB device became unreadable",
                *path,
                session.error().native_code));
        }
        if (!session->has_value() || **session != query.session_id) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::DeviceSnapshot,
                "IOUSB device session changed after exact-match filtering",
                *path));
        }
        if (nodes.size() == kMaximumRegistryCandidates) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::DeviceEnumeration,
                "IOKit USB mapping exceeds the bounded candidate limit",
                *path));
        }

        auto entry_id = registry_entry_id(
            service.get(), MacUsbTopologyStage::DeviceSnapshot, *path);
        const auto location = read_location_id(
            service.get(), MacUsbTopologyStage::DeviceSnapshot, *path);
        const auto address = required_unsigned_number(
            service.get(),
            CFSTR(kUSBHostDevicePropertyAddress),
            kUSBHostDevicePropertyAddress,
            std::numeric_limits<std::uint8_t>::max(),
            MacUsbTopologyStage::DeviceSnapshot,
            *path);
        const auto vendor = required_unsigned_number(
            service.get(),
            CFSTR(kUSBHostMatchingPropertyVendorID),
            kUSBHostMatchingPropertyVendorID,
            std::numeric_limits<std::uint16_t>::max(),
            MacUsbTopologyStage::DeviceSnapshot,
            *path);
        const auto product = required_unsigned_number(
            service.get(),
            CFSTR(kUSBHostMatchingPropertyProductID),
            kUSBHostMatchingPropertyProductID,
            std::numeric_limits<std::uint16_t>::max(),
            MacUsbTopologyStage::DeviceSnapshot,
            *path);
        if (!entry_id.has_value()) {
            return std::unexpected(entry_id.error());
        }
        if (!location.has_value()) {
            return std::unexpected(location.error());
        }
        if (!location->has_value()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::DeviceSnapshot,
                "session-matched IOUSB device has no locationID",
                *path));
        }
        if (!address.has_value()) {
            return std::unexpected(address.error());
        }
        if (!vendor.has_value()) {
            return std::unexpected(vendor.error());
        }
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        if (**session == 0U || *address == 0U || *vendor == 0U ||
            *product == 0U) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::DeviceSnapshot,
                "IOKit device identity contains a forbidden zero value",
                *path));
        }

        const auto hierarchy = read_hierarchy(
            service.get(), **session, deadline, cancellation);
        if (!hierarchy.has_value()) {
            return std::unexpected(hierarchy.error());
        }
        const auto serial = read_compatible_text(
            service.get(),
            {{{CFSTR(kUSBHostDevicePropertySerialNumberString),
               kUSBHostDevicePropertySerialNumberString},
              {CFSTR("USB Serial Number"), "USB Serial Number"}}},
            MacUsbTopologyStage::DeviceSnapshot,
            *path);
        const auto product_text = read_compatible_text(
            service.get(),
            {{{CFSTR(kUSBHostDevicePropertyProductString),
               kUSBHostDevicePropertyProductString},
              {CFSTR("USB Product Name"), "USB Product Name"}}},
            MacUsbTopologyStage::DeviceSnapshot,
            *path);
        if (!serial.has_value()) {
            return std::unexpected(serial.error());
        }
        if (!product_text.has_value()) {
            return std::unexpected(product_text.error());
        }
        auto interfaces = read_interfaces(
            service.get(), *entry_id, *path, deadline, cancellation);
        if (!interfaces.has_value()) {
            return std::unexpected(interfaces.error());
        }

        if (!IOIteratorIsValid(iterator)) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry device iterator was invalidated while reading the matched device",
                *path));
        }
        const auto final_session = read_session_id(
            service.get(), MacUsbTopologyStage::FinalValidation, *path);
        if (!final_session.has_value()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry device session became unreadable during revalidation",
                *path,
                final_session.error().native_code));
        }
        const auto final_entry_id = registry_entry_id(
            service.get(), MacUsbTopologyStage::FinalValidation, *path);
        if (!final_entry_id.has_value()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry entry identity became unreadable during revalidation",
                *path,
                final_entry_id.error().native_code));
        }
        if (!final_session->has_value() || **final_session != **session ||
            **final_session != query.session_id ||
            *final_entry_id != *entry_id) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry device identity changed while building its snapshot",
                *path));
        }
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }

        nodes.push_back(MacUsbRegistryNode{
            .registry_entry_id = *entry_id,
            .session_id = **session,
            .location_id = **location,
            .vendor_id = static_cast<std::uint16_t>(*vendor),
            .product_id = static_cast<std::uint16_t>(*product),
            .bus_number = static_cast<std::uint8_t>(**location >> 24U),
            .device_address = static_cast<std::uint8_t>(*address),
            .port_numbers = hierarchy->port_numbers,
            .serial_utf8 = *serial,
            .product_utf8 = *product_text,
            .registry_path = std::move(*path),
            .service_ancestry = std::move(hierarchy->service_ancestry),
            .interfaces = std::move(*interfaces),
        });
    }
    if (!IOIteratorIsValid(iterator)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::IdentityChanged,
            MacUsbTopologyStage::DeviceEnumeration,
            "IORegistry device iterator was invalidated during enumeration"));
    }
    if (const auto interrupted = interruption_error(
            deadline,
            cancellation,
            MacUsbTopologyStage::FinalValidation)) {
        return std::unexpected(*interrupted);
    }
    normalize_snapshot(nodes);
    return nodes;
}

#endif  // defined(__APPLE__)

}  // namespace

IokitMacUsbRegistryBackend::IokitMacUsbRegistryBackend(
    const IMacUsbRegistrySnapshotSource& source) noexcept
    : source_(&source) {}

std::expected<std::vector<MacUsbRegistryNode>, MacUsbTopologyError>
IokitMacUsbRegistryBackend::snapshot(
    const MacUsbTopologyQuery& query,
    const MacUsbTopologyTimePoint deadline,
    const std::stop_token cancellation) const {
    try {
        if (!query_is_valid(query)) {
            return std::unexpected(make_error(
                query.port_numbers.size() > kMaximumMacUsbTopologyDepth
                    ? MacUsbTopologyErrorKind::TopologyTooDeep
                    : MacUsbTopologyErrorKind::InvalidArgument,
                MacUsbTopologyStage::Validation,
                "invalid libusb identity for macOS topology discovery"));
        }
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::DeviceEnumeration)) {
            return std::unexpected(*interrupted);
        }
        std::expected<std::vector<MacUsbRegistryNode>, MacUsbTopologyError>
            result;
        if (source_ != nullptr) {
            result = source_->snapshot(query, deadline, cancellation);
        } else {
#if defined(__APPLE__)
            result = iokit_snapshot(query, deadline, cancellation);
#else
            result = std::unexpected(make_error(
                MacUsbTopologyErrorKind::UnsupportedPlatform,
                MacUsbTopologyStage::DeviceEnumeration,
                "IOKit USB topology discovery is unavailable on this platform"));
#endif
        }
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }
        if (result->size() > kMaximumRegistryCandidates) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry snapshot source exceeded the bounded candidate limit"));
        }
        normalize_snapshot(*result);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::ResourceExhausted,
            MacUsbTopologyStage::DeviceEnumeration,
            "memory allocation failed during IOKit USB enumeration"));
    }
}

MacUsbTopologyDiscovery::MacUsbTopologyDiscovery(
    const IMacUsbRegistryBackend& backend) noexcept
    : backend_(backend) {}

std::expected<MacUsbTopology, MacUsbTopologyError>
MacUsbTopologyDiscovery::discover(
    const MacUsbTopologyQuery& query,
    const MacUsbTopologyTimePoint deadline,
    const std::stop_token cancellation) const {
    auto resolved = discover_device(
        std::span<const MacUsbTopologyQuery>{&query, 1U},
        deadline,
        cancellation);
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }
    return std::move(resolved->front());
}

std::expected<std::vector<MacUsbTopology>, MacUsbTopologyError>
MacUsbTopologyDiscovery::discover_device(
    const std::span<const MacUsbTopologyQuery> queries,
    const MacUsbTopologyTimePoint deadline,
    const std::stop_token cancellation) const {
    try {
        if (queries.empty()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::InvalidArgument,
                MacUsbTopologyStage::Validation,
                "macOS topology device discovery requires at least one interface"));
        }
        if (queries.size() > kMaximumRegistryInterfaces) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::Validation,
                "macOS topology device discovery exceeds the interface limit"));
        }
        const auto& device_query = queries.front();
        for (std::size_t index = 0U; index < queries.size(); ++index) {
            if (const auto interrupted = interruption_error(
                    deadline, cancellation, MacUsbTopologyStage::Validation)) {
                return std::unexpected(*interrupted);
            }
            const auto& query = queries[index];
            if (!query_is_valid(query)) {
                return std::unexpected(make_error(
                    query.port_numbers.size() > kMaximumMacUsbTopologyDepth
                        ? MacUsbTopologyErrorKind::TopologyTooDeep
                        : MacUsbTopologyErrorKind::InvalidArgument,
                    MacUsbTopologyStage::Validation,
                    "invalid libusb identity for macOS topology discovery"));
            }
            const bool same_device =
                query.vendor_id == device_query.vendor_id &&
                query.product_id == device_query.product_id &&
                query.bus_number == device_query.bus_number &&
                query.device_address == device_query.device_address &&
                query.session_id == device_query.session_id &&
                query.port_numbers == device_query.port_numbers &&
                query.serial_utf8 == device_query.serial_utf8 &&
                query.product_utf8 == device_query.product_utf8 &&
                query.interface_fingerprint.configuration_value ==
                    device_query.interface_fingerprint.configuration_value;
            if (!same_device) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::InvalidArgument,
                    MacUsbTopologyStage::Validation,
                    "macOS topology batch mixes more than one libusb device identity"));
            }
            for (std::size_t previous = 0U; previous < index; ++previous) {
                if (queries[previous].interface_fingerprint ==
                    query.interface_fingerprint) {
                    return std::unexpected(make_error(
                        MacUsbTopologyErrorKind::AmbiguousMapping,
                        MacUsbTopologyStage::Validation,
                        "macOS topology batch repeats an interface fingerprint"));
                }
            }
        }
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::Validation)) {
            return std::unexpected(*interrupted);
        }

        auto first = backend_.snapshot(device_query, deadline, cancellation);
        if (!first.has_value()) {
            return std::unexpected(first.error());
        }
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }
        auto second = backend_.snapshot(device_query, deadline, cancellation);
        if (!second.has_value()) {
            return std::unexpected(second.error());
        }
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }
        if (first->size() > kMaximumRegistryCandidates ||
            second->size() > kMaximumRegistryCandidates) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry backend exceeded the bounded candidate limit"));
        }
        normalize_snapshot(*first);
        normalize_snapshot(*second);
        if (*first != *second) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::FinalValidation,
                "IORegistry USB identity changed between validation snapshots"));
        }

        const MacUsbRegistryNode* match = nullptr;
        const MacUsbRegistryAncestor* matched_controller = nullptr;
        bool identity_mismatch = false;
        bool saw_session = false;
        for (const auto& candidate : *second) {
            if (const auto interrupted = interruption_error(
                    deadline,
                    cancellation,
                    MacUsbTopologyStage::Correlation)) {
                return std::unexpected(*interrupted);
            }
            if (candidate.session_id != device_query.session_id) {
                continue;
            }
            if (!node_shape_is_valid(candidate)) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Correlation,
                    "IORegistry backend returned a malformed USB node",
                    candidate.registry_path));
            }
            if (saw_session) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::AmbiguousMapping,
                    MacUsbTopologyStage::Correlation,
                    "more than one IORegistry device has the libusb session identity",
                    candidate.registry_path));
            }
            saw_session = true;
            const bool same_physical_port =
                candidate.bus_number == device_query.bus_number &&
                candidate.port_numbers == device_query.port_numbers;
            if (!same_physical_port ||
                candidate.device_address != device_query.device_address ||
                candidate.vendor_id != device_query.vendor_id ||
                candidate.product_id != device_query.product_id ||
                (device_query.serial_utf8.has_value() &&
                 (!candidate.serial_utf8.has_value() ||
                  *candidate.serial_utf8 != *device_query.serial_utf8)) ||
                (device_query.product_utf8.has_value() &&
                 (!candidate.product_utf8.has_value() ||
                  *candidate.product_utf8 != *device_query.product_utf8))) {
                identity_mismatch = true;
                continue;
            }

            const auto* controller = root_controller(candidate);
            if (controller == nullptr) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Correlation,
                    "IORegistry ancestry contains no USB host controller",
                    candidate.registry_path));
            }

            if (match != nullptr) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::AmbiguousMapping,
                    MacUsbTopologyStage::Correlation,
                    "more than one IORegistry device maps to the libusb snapshot",
                    candidate.registry_path));
            }
            match = &candidate;
            matched_controller = controller;
        }
        if (match == nullptr || matched_controller == nullptr) {
            return std::unexpected(make_error(
                identity_mismatch ? MacUsbTopologyErrorKind::IdentityMismatch
                                  : MacUsbTopologyErrorKind::NotFound,
                MacUsbTopologyStage::Correlation,
                identity_mismatch
                    ? "IORegistry USB identity does not match the libusb snapshot"
                    : "no IORegistry device maps to the libusb snapshot"));
        }

        auto physical_path = canonical_macos_usb_port_path(
            match->bus_number, match->port_numbers);
        if (!physical_path.has_value()) {
            return std::unexpected(physical_path.error());
        }
        auto controller_id = controller_identifier(
            matched_controller->registry_entry_id);
        if (controller_id.empty()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::Correlation,
                "failed to format the root-controller registry identity",
                matched_controller->registry_path));
        }
        if (const auto interrupted = interruption_error(
                deadline,
                cancellation,
                MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }

        std::vector<MacUsbTopology> topologies;
        topologies.reserve(queries.size());
        for (const auto& query : queries) {
            const MacUsbRegistryInterface* matched_interface = nullptr;
            for (const auto& interface : match->interfaces) {
                if (const auto interrupted = interruption_error(
                        deadline,
                        cancellation,
                        MacUsbTopologyStage::Correlation)) {
                    return std::unexpected(*interrupted);
                }
                if (interface.fingerprint != query.interface_fingerprint) {
                    continue;
                }
                if (matched_interface != nullptr) {
                    return std::unexpected(make_error(
                        MacUsbTopologyErrorKind::AmbiguousMapping,
                        MacUsbTopologyStage::Correlation,
                        "more than one IORegistry interface matches the libusb fingerprint",
                        interface.registry_path));
                }
                matched_interface = &interface;
            }
            if (matched_interface == nullptr) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::IdentityMismatch,
                    MacUsbTopologyStage::Correlation,
                    "an IORegistry interface does not match the libusb device snapshot",
                    match->registry_path));
            }
            topologies.push_back(MacUsbTopology{
                .physical_port_path = *physical_path,
                .root_controller_id = controller_id,
                .hub_port_chain = match->port_numbers,
                .registry_entry_id = match->registry_entry_id,
                .session_id = match->session_id,
                .interface_registry_entry_id =
                    matched_interface->registry_entry_id,
                .location_id = match->location_id,
                .vendor_id = match->vendor_id,
                .product_id = match->product_id,
                .bus_number = match->bus_number,
                .device_address = match->device_address,
                .interface_fingerprint = matched_interface->fingerprint,
                .serial_utf8 = match->serial_utf8,
                .product_utf8 = match->product_utf8,
                .registry_path = match->registry_path,
                .interface_registry_path = matched_interface->registry_path,
                .root_controller_registry_path =
                    matched_controller->registry_path,
            });
        }
        return topologies;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::ResourceExhausted,
            MacUsbTopologyStage::Correlation,
            "memory allocation failed while correlating macOS USB topology"));
    }
}

MacUsbTopologyQuery make_macos_usb_topology_query(
    const UsbDeviceInfo& device,
    const std::optional<std::string_view> product_utf8) {
    return MacUsbTopologyQuery{
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .session_id = device.backend_session_id,
        .port_numbers = device.port_path,
        .interface_fingerprint = MacUsbInterfaceFingerprint{
            .configuration_value = device.configuration_value,
            .interface_number = device.interface_number,
            .alternate_setting = device.alternate_setting,
            .interface_class = device.interface_class,
            .interface_subclass = device.interface_subclass,
            .interface_protocol = device.interface_protocol,
        },
        .serial_utf8 = device.serial_utf8.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{device.serial_utf8},
        .product_utf8 = product_utf8.has_value() && !product_utf8->empty()
            ? std::optional<std::string>{*product_utf8}
            : std::optional<std::string>{},
    };
}

std::expected<std::string, MacUsbTopologyError>
canonical_macos_usb_port_path(
    const std::uint8_t bus_number,
    const std::vector<std::uint8_t>& port_numbers) {
    try {
        if (port_numbers.empty()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::InvalidArgument,
                MacUsbTopologyStage::Validation,
                "USB physical path requires a non-empty port chain"));
        }
        if (port_numbers.size() > kMaximumMacUsbTopologyDepth) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::TopologyTooDeep,
                MacUsbTopologyStage::Validation,
                "macOS USB hub depth exceeds the USB tier limit"));
        }
        if (std::ranges::any_of(port_numbers, [](const std::uint8_t port) {
                return port == 0U;
            })) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::InvalidArgument,
                MacUsbTopologyStage::Validation,
                "macOS USB physical path contains an invalid port number"));
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
            MacUsbTopologyErrorKind::ResourceExhausted,
            MacUsbTopologyStage::Validation,
            "memory allocation failed while formatting a macOS USB path"));
    }
}

}  // namespace kairosboot::transport
