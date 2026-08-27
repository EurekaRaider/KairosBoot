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
#if defined(__APPLE__)
constexpr std::size_t kMaximumRegistryAncestry = 64U;
#endif

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
        query.device_address != 0U && !query.port_numbers.empty() &&
        query.port_numbers.size() <= kMaximumMacUsbTopologyDepth &&
        std::ranges::all_of(query.port_numbers, [](const std::uint8_t port) {
            return port > 0U && port <= 0x0FU;
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

[[nodiscard]] std::expected<MacUsbDecodedLocation, MacUsbTopologyError>
decode_location(const std::uint32_t location_id, const bool require_port) {
    try {
        // Keep this bit layout aligned with locked libusb 1.0.30
        // libusb/os/darwin_usb.c: process_new_device() assigns
        // bus_number = location >> 24, while the remaining six nibbles encode
        // the downstream hub/port chain used by Darwin physical locations.
        MacUsbDecodedLocation decoded{
            .bus_number = static_cast<std::uint8_t>(location_id >> 24U),
        };
        bool reached_terminator = false;
        for (const unsigned int shift : {20U, 16U, 12U, 8U, 4U, 0U}) {
            const auto port =
                static_cast<std::uint8_t>((location_id >> shift) & 0x0FU);
            if (port == 0U) {
                reached_terminator = true;
                continue;
            }
            if (reached_terminator) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Hierarchy,
                    "IOKit locationID contains a non-zero port after a terminator"));
            }
            decoded.port_numbers.push_back(port);
        }
        if (require_port && decoded.port_numbers.empty()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::Hierarchy,
                "IOKit device locationID has no downstream port"));
        }
        return decoded;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::ResourceExhausted,
            MacUsbTopologyStage::Hierarchy,
            "memory allocation failed while decoding IOKit locationID"));
    }
}

[[nodiscard]] bool interface_shape_is_valid(
    const MacUsbRegistryInterface& interface) noexcept {
    return interface.registry_entry_id != 0U &&
        valid_registry_path(interface.registry_path);
}

[[nodiscard]] bool node_shape_is_valid(const MacUsbRegistryNode& node) {
    if (node.registry_entry_id == 0U || node.session_id == 0U ||
        node.root_controller_registry_entry_id == 0U ||
        node.root_controller_registry_entry_id == node.registry_entry_id ||
        node.vendor_id == 0U || node.product_id == 0U ||
        node.device_address == 0U || node.port_numbers.empty() ||
        node.port_numbers.size() > kMaximumMacUsbTopologyDepth ||
        node.interfaces.size() > kMaximumRegistryInterfaces ||
        !valid_optional_identity(node.serial_utf8) ||
        !valid_optional_identity(node.product_utf8) ||
        !valid_registry_path(node.registry_path) ||
        !valid_registry_path(node.root_controller_registry_path) ||
        !std::ranges::all_of(node.interfaces, interface_shape_is_valid)) {
        return false;
    }
    const auto decoded = decode_location(node.location_id, true);
    if (!decoded.has_value() || decoded->bus_number != node.bus_number ||
        decoded->port_numbers != node.port_numbers) {
        return false;
    }
    for (std::size_t index = 0; index < node.interfaces.size(); ++index) {
        const auto& interface = node.interfaces[index];
        if (interface.registry_entry_id == node.registry_entry_id ||
            interface.registry_entry_id ==
                node.root_controller_registry_entry_id) {
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

[[nodiscard]] bool is_prefix(const std::vector<std::uint8_t>& prefix,
                             const std::vector<std::uint8_t>& full) noexcept {
    return prefix.size() <= full.size() &&
        std::equal(prefix.begin(), prefix.end(), full.begin());
}

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

struct HierarchyResult final {
    std::uint64_t controller_entry_id{};
    std::string controller_path;
};

struct AncestorMetadata final {
    std::uint64_t entry_id{};
    std::string path;
    std::optional<std::uint64_t> session_id;
    std::optional<std::uint32_t> location_id;
};

[[nodiscard]] std::expected<HierarchyResult, MacUsbTopologyError>
read_hierarchy(const io_registry_entry_t device,
               const MacUsbDecodedLocation& leaf_location,
               const MacUsbTopologyTimePoint deadline,
               const std::stop_token cancellation) {
    std::vector<AncestorMetadata> ancestry;
    UniqueIoObject owned_current;
    io_registry_entry_t current = device;

    for (std::size_t depth = 0U; depth < kMaximumRegistryAncestry; ++depth) {
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::Hierarchy)) {
            return std::unexpected(*interrupted);
        }
        auto path = registry_path(current, kIOUSBPlane,
                                  MacUsbTopologyStage::Hierarchy);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        auto entry_id = registry_entry_id(
            current, MacUsbTopologyStage::Hierarchy, *path);
        if (!entry_id.has_value()) {
            return std::unexpected(entry_id.error());
        }
        auto session = read_unsigned_number(
            current,
            CFSTR("sessionID"),
            "sessionID",
            std::numeric_limits<std::int64_t>::max(),
            MacUsbTopologyStage::Hierarchy,
            *path);
        if (!session.has_value()) {
            return std::unexpected(session.error());
        }
        auto location = read_location_id(
            current, MacUsbTopologyStage::Hierarchy, *path);
        if (!location.has_value()) {
            return std::unexpected(location.error());
        }
        ancestry.push_back(AncestorMetadata{
            .entry_id = *entry_id,
            .path = std::move(*path),
            .session_id = *session,
            .location_id = *location,
        });

        io_registry_entry_t parent = IO_OBJECT_NULL;
        const auto parent_result = IORegistryEntryGetParentEntry(
            current, kIOUSBPlane, &parent);
        if (parent_result == kIOReturnNoDevice ||
            parent_result == kIOReturnNotFound) {
            break;
        }
        if (parent_result != kIOReturnSuccess || parent == IO_OBJECT_NULL) {
            return std::unexpected(native_error(
                MacUsbTopologyStage::Hierarchy,
                parent_result,
                ancestry.back().path,
                "failed to traverse the IOUSB registry hierarchy"));
        }
        owned_current = UniqueIoObject(parent);
        current = owned_current.get();
    }
    if (ancestry.size() == kMaximumRegistryAncestry) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::TopologyTooDeep,
            MacUsbTopologyStage::Hierarchy,
            "IOUSB registry ancestry exceeds the bounded traversal depth"));
    }
    if (ancestry.size() < 3U) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IOUSB device has no unique controller and registry root ancestors",
            ancestry.empty() ? std::string{} : ancestry.front().path));
    }

    const auto& controller = ancestry[ancestry.size() - 2U];
    if (controller.session_id.has_value() || !controller.location_id.has_value() ||
        static_cast<std::uint8_t>(*controller.location_id >> 24U) !=
            leaf_location.bus_number ||
        (*controller.location_id & 0x00FFFFFFU) != 0U) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IOUSB ancestry does not end at the expected root controller",
            controller.path));
    }

    std::size_t previous_depth = leaf_location.port_numbers.size() + 1U;
    bool saw_leaf = false;
    for (std::size_t index = 0U; index + 2U < ancestry.size(); ++index) {
        const auto& entry = ancestry[index];
        if (!entry.session_id.has_value()) {
            continue;
        }
        if (*entry.session_id == 0U || !entry.location_id.has_value()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::Hierarchy,
                "IOUSB device ancestor has incomplete session/location identity",
                entry.path));
        }
        const auto decoded = decode_location(*entry.location_id, true);
        if (!decoded.has_value()) {
            auto error = decoded.error();
            error.registry_path = entry.path;
            return std::unexpected(std::move(error));
        }
        if (decoded->bus_number != leaf_location.bus_number ||
            !is_prefix(decoded->port_numbers, leaf_location.port_numbers) ||
            decoded->port_numbers.size() >= previous_depth) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::Hierarchy,
                "IOUSB hub ancestry is inconsistent with the device locationID",
                entry.path));
        }
        if (!saw_leaf && decoded->port_numbers != leaf_location.port_numbers) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::IdentityChanged,
                MacUsbTopologyStage::Hierarchy,
                "IOUSB leaf location changed while its hierarchy was read",
                entry.path));
        }
        saw_leaf = true;
        previous_depth = decoded->port_numbers.size();
    }
    if (!saw_leaf) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::MalformedRegistry,
            MacUsbTopologyStage::Hierarchy,
            "IOUSB hierarchy contains no session-bearing leaf device",
            ancestry.front().path));
    }
    return HierarchyResult{
        .controller_entry_id = controller.entry_id,
        .controller_path = controller.path,
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
        if (IOObjectConformsTo(current, kIOUSBHostDeviceClassName)) {
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
    if (iterator_result != kIOReturnSuccess || iterator == IO_OBJECT_NULL) {
        return std::unexpected(native_error(
            MacUsbTopologyStage::InterfaceEnumeration,
            iterator_result,
            device_path,
            "failed to enumerate USB interfaces below an IORegistry device"));
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
        if (!IOObjectConformsTo(service.get(), kIOUSBHostInterfaceClassName)) {
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
    auto* matching = IOServiceMatching(kIOUSBHostDeviceClassName);
    if (matching == nullptr) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::ResourceExhausted,
            MacUsbTopologyStage::DeviceEnumeration,
            "IOKit could not allocate a USB device matching dictionary"));
    }
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto iterator_result = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iterator);
    if (iterator_result != kIOReturnSuccess || iterator == IO_OBJECT_NULL) {
        return std::unexpected(native_error(
            MacUsbTopologyStage::DeviceEnumeration,
            iterator_result,
            {},
            "failed to enumerate IOUSBHostDevice services"));
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
        auto path = registry_path(service.get(),
                                  kIOUSBPlane,
                                  MacUsbTopologyStage::DeviceSnapshot);
        if (!path.has_value()) {
            return std::unexpected(path.error());
        }
        const auto location = read_location_id(
            service.get(), MacUsbTopologyStage::DeviceSnapshot, *path);
        if (!location.has_value()) {
            return std::unexpected(location.error());
        }
        if (!location->has_value()) {
            continue;
        }
        const auto decoded = decode_location(**location, true);
        if (!decoded.has_value()) {
            if (static_cast<std::uint8_t>(**location >> 24U) ==
                query.bus_number) {
                auto error = decoded.error();
                error.registry_path = *path;
                return std::unexpected(std::move(error));
            }
            continue;
        }
        if (decoded->bus_number != query.bus_number ||
            decoded->port_numbers != query.port_numbers) {
            continue;
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
        const auto session = required_unsigned_number(
            service.get(),
            CFSTR("sessionID"),
            "sessionID",
            std::numeric_limits<std::int64_t>::max(),
            MacUsbTopologyStage::DeviceSnapshot,
            *path);
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
        if (!session.has_value()) {
            return std::unexpected(session.error());
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
        if (*session == 0U || *address == 0U || *vendor == 0U || *product == 0U) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::MalformedRegistry,
                MacUsbTopologyStage::DeviceSnapshot,
                "IOKit device identity contains a forbidden zero value",
                *path));
        }

        const auto hierarchy = read_hierarchy(
            service.get(), *decoded, deadline, cancellation);
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

        nodes.push_back(MacUsbRegistryNode{
            .registry_entry_id = *entry_id,
            .session_id = *session,
            .root_controller_registry_entry_id =
                hierarchy->controller_entry_id,
            .location_id = **location,
            .vendor_id = static_cast<std::uint16_t>(*vendor),
            .product_id = static_cast<std::uint16_t>(*product),
            .bus_number = decoded->bus_number,
            .device_address = static_cast<std::uint8_t>(*address),
            .port_numbers = decoded->port_numbers,
            .serial_utf8 = *serial,
            .product_utf8 = *product_text,
            .registry_path = std::move(*path),
            .root_controller_registry_path = hierarchy->controller_path,
            .interfaces = std::move(*interfaces),
        });
    }
    if (!IOIteratorIsValid(iterator)) {
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::IdentityChanged,
            MacUsbTopologyStage::DeviceEnumeration,
            "IORegistry device iterator was invalidated during enumeration"));
    }
    normalize_snapshot(nodes);
    return nodes;
}

#endif  // defined(__APPLE__)

}  // namespace

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
#if defined(__APPLE__)
        return iokit_snapshot(query, deadline, cancellation);
#else
        (void)deadline;
        (void)cancellation;
        return std::unexpected(make_error(
            MacUsbTopologyErrorKind::UnsupportedPlatform,
            MacUsbTopologyStage::DeviceEnumeration,
            "IOKit USB topology discovery is unavailable on this platform"));
#endif
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
                deadline, cancellation, MacUsbTopologyStage::Validation)) {
            return std::unexpected(*interrupted);
        }

        auto first = backend_.snapshot(query, deadline, cancellation);
        if (!first.has_value()) {
            return std::unexpected(first.error());
        }
        if (const auto interrupted = interruption_error(
                deadline, cancellation, MacUsbTopologyStage::FinalValidation)) {
            return std::unexpected(*interrupted);
        }
        auto second = backend_.snapshot(query, deadline, cancellation);
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
        const MacUsbRegistryInterface* matched_interface = nullptr;
        bool identity_mismatch = false;
        for (const auto& candidate : *second) {
            if (!node_shape_is_valid(candidate)) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::MalformedRegistry,
                    MacUsbTopologyStage::Correlation,
                    "IORegistry backend returned a malformed USB node",
                    candidate.registry_path));
            }
            const bool same_physical_port =
                candidate.bus_number == query.bus_number &&
                candidate.port_numbers == query.port_numbers;
            if (!same_physical_port) {
                continue;
            }
            if (candidate.device_address != query.device_address ||
                candidate.vendor_id != query.vendor_id ||
                candidate.product_id != query.product_id ||
                (query.serial_utf8.has_value() &&
                 (!candidate.serial_utf8.has_value() ||
                  *candidate.serial_utf8 != *query.serial_utf8)) ||
                (query.product_utf8.has_value() &&
                 (!candidate.product_utf8.has_value() ||
                  *candidate.product_utf8 != *query.product_utf8))) {
                identity_mismatch = true;
                continue;
            }

            const MacUsbRegistryInterface* interface_match = nullptr;
            for (const auto& interface : candidate.interfaces) {
                if (interface.fingerprint != query.interface_fingerprint) {
                    continue;
                }
                if (interface_match != nullptr) {
                    return std::unexpected(make_error(
                        MacUsbTopologyErrorKind::AmbiguousMapping,
                        MacUsbTopologyStage::Correlation,
                        "more than one IORegistry interface matches the libusb fingerprint",
                        interface.registry_path));
                }
                interface_match = &interface;
            }
            if (interface_match == nullptr) {
                identity_mismatch = true;
                continue;
            }
            if (match != nullptr) {
                return std::unexpected(make_error(
                    MacUsbTopologyErrorKind::AmbiguousMapping,
                    MacUsbTopologyStage::Correlation,
                    "more than one IORegistry device maps to the libusb snapshot",
                    candidate.registry_path));
            }
            match = &candidate;
            matched_interface = interface_match;
        }
        if (match == nullptr || matched_interface == nullptr) {
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
            match->root_controller_registry_entry_id);
        if (controller_id.empty()) {
            return std::unexpected(make_error(
                MacUsbTopologyErrorKind::ResourceExhausted,
                MacUsbTopologyStage::Correlation,
                "failed to format the root-controller registry identity",
                match->root_controller_registry_path));
        }
        return MacUsbTopology{
            .physical_port_path = std::move(*physical_path),
            .root_controller_id = std::move(controller_id),
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
                match->root_controller_registry_path,
        };
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
                "macOS USB hub depth exceeds the IOKit locationID limit"));
        }
        if (std::ranges::any_of(port_numbers, [](const std::uint8_t port) {
                return port == 0U || port > 0x0FU;
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

std::expected<MacUsbDecodedLocation, MacUsbTopologyError>
decode_macos_usb_location_id(const std::uint32_t location_id) {
    return decode_location(location_id, true);
}

}  // namespace kairosboot::transport
