// SPDX-License-Identifier: MIT
#include "src/transport/linux_usb_topology.hpp"

#include "src/transport/libusb_runtime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <limits>
#include <memory>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kairosboot::transport {
namespace {

constexpr std::size_t kMaximumSysfsPathBytes = 4U * 1024U;
constexpr std::size_t kMaximumSysfsComponents = 128U;
constexpr std::size_t kMaximumAttributeBytes = 512U;
constexpr std::size_t kMaximumIdentityTextBytes = 256U;

[[nodiscard]] LinuxUsbTopologyError make_error(
    const LinuxUsbTopologyErrorKind kind,
    const LinuxUsbTopologyStage stage,
    std::string message,
    std::string path = {},
    const int native_code = 0) {
    return LinuxUsbTopologyError{
        .kind = kind,
        .stage = stage,
        .native_code = native_code,
        .path = std::move(path),
        .message = std::move(message),
    };
}

#if !defined(_WIN32)
[[nodiscard]] LinuxUsbTopologyError native_error(
    const LinuxUsbTopologyStage stage,
    const int native_code,
    std::string path,
    std::string message) {
    const auto kind = native_code == EACCES || native_code == EPERM
        ? LinuxUsbTopologyErrorKind::PermissionDenied
        : native_code == ENOENT || native_code == ENODEV
            ? LinuxUsbTopologyErrorKind::NotFound
            : native_code == ELOOP || native_code == ENOTDIR || native_code == EINVAL
                ? LinuxUsbTopologyErrorKind::UnsafePath
                : LinuxUsbTopologyErrorKind::IoError;
    return make_error(kind,
                      stage,
                      std::move(message),
                      std::move(path),
                      native_code);
}
#endif

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
            continuation_count = 1;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation <= continuation_count;
             ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }
        if ((continuation_count == 1 && code_point < 0x80U) ||
            (continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
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

[[nodiscard]] bool valid_component(const std::string_view component) noexcept {
    if (component.empty() || component == "." || component == ".." ||
        component.size() > 255U) {
        return false;
    }
    return std::ranges::all_of(component, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21U && byte != 0x7FU && character != '/';
    });
}

[[nodiscard]] bool valid_slash_path(const std::string_view path,
                                    const std::string_view required_first = {},
                                    const std::string_view required_last = {}) noexcept {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return false;
    }
    std::size_t start = 0;
    std::size_t count = 0;
    std::string_view first;
    std::string_view last;
    while (start <= path.size()) {
        const auto separator = path.find('/', start);
        const auto component = path.substr(
            start,
            separator == std::string_view::npos ? std::string_view::npos
                                                : separator - start);
        if (!valid_component(component) || count == kMaximumSysfsComponents) {
            return false;
        }
        if (count == 0U) {
            first = component;
        }
        last = component;
        ++count;
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return (required_first.empty() || first == required_first) &&
        (required_last.empty() || last == required_last);
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_integer(
    const std::string_view text,
    const int base,
    const Integer maximum = std::numeric_limits<Integer>::max()) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return std::nullopt;
    }
    unsigned long long parsed = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed, base);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed > static_cast<unsigned long long>(maximum)) {
        return std::nullopt;
    }
    return static_cast<Integer>(parsed);
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> parse_port_chain(
    const std::string_view value) {
    std::vector<std::uint8_t> ports;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto separator = value.find('.', start);
        const auto token = value.substr(
            start,
            separator == std::string_view::npos ? std::string_view::npos
                                                : separator - start);
        const auto port = parse_integer<std::uint8_t>(token, 10);
        if (!port.has_value() || *port == 0U ||
            ports.size() == kMaximumUsbTopologyDepth) {
            return std::nullopt;
        }
        ports.push_back(*port);
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return ports.empty() ? std::nullopt
                         : std::optional<std::vector<std::uint8_t>>{std::move(ports)};
}

[[nodiscard]] std::expected<std::string, LinuxUsbTopologyError>
device_entry_name(const LinuxUsbTopologyQuery& query) {
    auto path = canonical_linux_usb_port_path(query.bus_number, query.port_numbers);
    if (!path.has_value()) {
        return std::unexpected(path.error());
    }
    return path->substr(std::string_view{"usb:"}.size());
}

[[nodiscard]] bool query_is_valid(const LinuxUsbTopologyQuery& query) noexcept {
    if (query.vendor_id == 0U || query.product_id == 0U ||
        query.bus_number == 0U || query.device_address == 0U ||
        query.port_numbers.empty() ||
        query.port_numbers.size() > kMaximumUsbTopologyDepth ||
        std::ranges::any_of(query.port_numbers, [](const std::uint8_t port) {
            return port == 0U;
        })) {
        return false;
    }
    const auto valid_optional = [](const std::optional<std::string>& text) {
        return !text.has_value() || valid_identity_text(*text);
    };
    return valid_optional(query.serial_utf8) && valid_optional(query.product_utf8);
}

[[nodiscard]] bool node_shape_is_valid(const LinuxUsbSysfsNode& node) {
    if (!valid_component(node.entry_name) || node.root_relative_path.empty() ||
        node.root_relative_path.size() > kMaximumSysfsPathBytes ||
        !valid_slash_path(node.root_relative_path, "devices", node.entry_name) ||
        node.root_controller_id.empty() ||
        node.root_controller_id.size() > kMaximumSysfsPathBytes ||
        !node.root_controller_id.starts_with("linux-sysfs:") ||
        node.vendor_id == 0U || node.product_id == 0U ||
        node.bus_number == 0U || node.device_address == 0U ||
        node.port_numbers.empty() ||
        node.port_numbers.size() > kMaximumUsbTopologyDepth ||
        std::ranges::any_of(node.port_numbers, [](const std::uint8_t port) {
            return port == 0U;
        })) {
        return false;
    }
    const auto controller_path = std::string_view(node.root_controller_id)
                                     .substr(std::string_view{"linux-sysfs:"}.size());
    if (!valid_slash_path(controller_path)) {
        return false;
    }
    std::string expected_path = "devices/";
    expected_path.append(controller_path);
    expected_path += "/usb" + std::to_string(node.bus_number);
    std::string expected_entry = std::to_string(node.bus_number) + "-";
    for (std::size_t index = 0; index < node.port_numbers.size(); ++index) {
        if (index != 0U) {
            expected_entry.push_back('.');
        }
        expected_entry += std::to_string(node.port_numbers[index]);
        expected_path.push_back('/');
        expected_path += expected_entry;
    }
    if (node.entry_name != expected_entry ||
        node.root_relative_path != expected_path) {
        return false;
    }
    const auto valid_optional = [](const std::optional<std::string>& text) {
        return !text.has_value() || valid_identity_text(*text);
    };
    return valid_optional(node.serial_utf8) && valid_optional(node.product_utf8);
}

#if !defined(_WIN32)

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(const int descriptor) noexcept : descriptor_(descriptor) {}
    ~UniqueFd() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

private:
    int descriptor_{-1};
};

struct DirectoryIdentity final {
    std::uintmax_t device{};
    std::uintmax_t inode{};

    [[nodiscard]] bool operator==(const DirectoryIdentity&) const = default;
};

[[nodiscard]] std::expected<DirectoryIdentity, LinuxUsbTopologyError>
directory_identity(const int descriptor,
                   const LinuxUsbTopologyStage stage,
                   const std::string& display_path) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        const auto error = errno;
        return std::unexpected(native_error(
            stage,
            error,
            display_path,
            "failed to inspect an opened sysfs directory"));
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            stage,
            "opened sysfs object is not a directory",
            display_path));
    }
    return DirectoryIdentity{
        .device = static_cast<std::uintmax_t>(status.st_dev),
        .inode = static_cast<std::uintmax_t>(status.st_ino),
    };
}

[[nodiscard]] std::expected<UniqueFd, LinuxUsbTopologyError>
open_absolute_root(const std::string_view root) {
    if (root.empty() || root.front() != '/' ||
        root.size() > kMaximumSysfsPathBytes ||
        (root.size() > 1U && root.back() == '/')) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            LinuxUsbTopologyStage::RootOpen,
            "sysfs root must be an absolute bounded path",
            std::string(root)));
    }

    UniqueFd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current) {
        const auto error = errno;
        return std::unexpected(native_error(
            LinuxUsbTopologyStage::RootOpen,
            error,
            "/",
            "failed to open filesystem root"));
    }

    std::size_t start = 1U;
    std::size_t component_count = 0U;
    while (start < root.size()) {
        const auto separator = root.find('/', start);
        const auto component = root.substr(
            start,
            separator == std::string_view::npos ? std::string_view::npos
                                                : separator - start);
        if (!valid_component(component) ||
            component_count == kMaximumSysfsComponents) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::UnsafePath,
                LinuxUsbTopologyStage::RootOpen,
                "sysfs root contains an unsafe path component",
                std::string(root)));
        }
        ++component_count;
        const std::string component_name(component);
        const auto next = ::openat(current.get(),
                                   component_name.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const auto error = errno;
            return std::unexpected(native_error(
                LinuxUsbTopologyStage::RootOpen,
                error,
                std::string(root),
                "failed to open sysfs root without following symlinks"));
        }
        current = UniqueFd(next);
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return current;
}

[[nodiscard]] std::expected<UniqueFd, LinuxUsbTopologyError> open_directory_at(
    const int parent,
    const std::string_view component,
    const LinuxUsbTopologyStage stage,
    std::string path) {
    if (!valid_component(component)) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            stage,
            "unsafe sysfs directory component",
            std::move(path)));
    }
    const std::string name(component);
    const auto descriptor = ::openat(parent,
                                     name.c_str(),
                                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        const auto error = errno;
        return std::unexpected(native_error(stage,
                                            error,
                                            std::move(path),
                                            "failed to open sysfs directory"));
    }
    return UniqueFd(descriptor);
}

[[nodiscard]] std::expected<UniqueFd, LinuxUsbTopologyError>
open_usb_devices_directory(const int root) {
    auto bus = open_directory_at(root,
                                 "bus",
                                 LinuxUsbTopologyStage::Lookup,
                                 "bus");
    if (!bus.has_value()) {
        return std::unexpected(bus.error());
    }
    auto usb = open_directory_at(bus->get(),
                                 "usb",
                                 LinuxUsbTopologyStage::Lookup,
                                 "bus/usb");
    if (!usb.has_value()) {
        return std::unexpected(usb.error());
    }
    return open_directory_at(usb->get(),
                             "devices",
                             LinuxUsbTopologyStage::Lookup,
                             "bus/usb/devices");
}

[[nodiscard]] std::expected<std::string, LinuxUsbTopologyError> read_link_at(
    const int parent,
    const std::string& name,
    const std::string& display_path) {
    std::array<char, kMaximumSysfsPathBytes + 1U> buffer{};
    const auto bytes = ::readlinkat(parent, name.c_str(), buffer.data(),
                                    kMaximumSysfsPathBytes);
    if (bytes < 0) {
        const auto error = errno;
        return std::unexpected(native_error(
            LinuxUsbTopologyStage::SymlinkResolution,
            error,
            display_path,
            "failed to read the USB sysfs link without following it"));
    }
    if (bytes == 0 || static_cast<std::size_t>(bytes) >= kMaximumSysfsPathBytes) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            LinuxUsbTopologyStage::SymlinkResolution,
            "USB sysfs link target is empty or too long",
            display_path));
    }
    return std::string(buffer.data(), static_cast<std::size_t>(bytes));
}

[[nodiscard]] std::expected<std::vector<std::string>, LinuxUsbTopologyError>
normalize_link_target(const std::string_view target,
                      const std::string& expected_entry,
                      const std::string& display_path) {
    if (target.empty() || target.front() == '/') {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            LinuxUsbTopologyStage::SymlinkResolution,
            "USB sysfs link target must be relative",
            display_path));
    }

    std::vector<std::string> components{"bus", "usb", "devices"};
    std::size_t start = 0;
    while (start <= target.size()) {
        const auto separator = target.find('/', start);
        const auto component = target.substr(
            start,
            separator == std::string_view::npos ? std::string_view::npos
                                                : separator - start);
        if (component.empty() || component == ".") {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::UnsafePath,
                LinuxUsbTopologyStage::SymlinkResolution,
                "USB sysfs link target contains an empty or current-directory component",
                display_path));
        }
        if (component == "..") {
            if (components.empty()) {
                return std::unexpected(make_error(
                    LinuxUsbTopologyErrorKind::UnsafePath,
                    LinuxUsbTopologyStage::SymlinkResolution,
                    "USB sysfs link target escapes the configured sysfs root",
                    display_path));
            }
            components.pop_back();
        } else {
            if (!valid_component(component) ||
                components.size() == kMaximumSysfsComponents) {
                return std::unexpected(make_error(
                    LinuxUsbTopologyErrorKind::UnsafePath,
                    LinuxUsbTopologyStage::SymlinkResolution,
                    "USB sysfs link target contains an unsafe component",
                    display_path));
            }
            components.emplace_back(component);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }

    if (components.size() < 3U || components.front() != "devices" ||
        components.back() != expected_entry) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            LinuxUsbTopologyStage::SymlinkResolution,
            "USB sysfs link does not resolve to the expected device beneath /devices",
            display_path));
    }
    return components;
}

[[nodiscard]] std::string join_components(
    const std::span<const std::string> components) {
    std::string result;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (index != 0U) {
            result.push_back('/');
        }
        result += components[index];
    }
    return result;
}

[[nodiscard]] std::expected<UniqueFd, LinuxUsbTopologyError>
open_resolved_device(const int root,
                     const std::vector<std::string>& components,
                     const std::string& display_path) {
    UniqueFd current;
    auto parent = root;
    for (const auto& component : components) {
        auto next = open_directory_at(parent,
                                      component,
                                      LinuxUsbTopologyStage::SymlinkResolution,
                                      display_path);
        if (!next.has_value()) {
            return std::unexpected(next.error());
        }
        current = std::move(*next);
        parent = current.get();
    }
    return current;
}

[[nodiscard]] std::expected<std::string, LinuxUsbTopologyError> read_attribute(
    const int device,
    const std::string_view name,
    const std::string& display_path) {
    const std::string attribute(name);
    UniqueFd descriptor(::openat(device,
                                 attribute.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!descriptor) {
        const auto error = errno;
        return std::unexpected(native_error(
            LinuxUsbTopologyStage::AttributeRead,
            error,
            display_path + "/" + attribute,
            "failed to open a sysfs attribute without following symlinks"));
    }

    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0) {
        const auto error = errno;
        return std::unexpected(native_error(
            LinuxUsbTopologyStage::AttributeRead,
            error,
            display_path + "/" + attribute,
            "failed to inspect a sysfs attribute"));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::UnsafePath,
            LinuxUsbTopologyStage::AttributeRead,
            "sysfs attribute is not a regular virtual file",
            display_path + "/" + attribute));
    }

    std::string value;
    value.reserve(64U);
    std::array<char, 128U> buffer{};
    for (;;) {
        const auto bytes = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto error = errno;
            return std::unexpected(native_error(
                LinuxUsbTopologyStage::AttributeRead,
                error,
                display_path + "/" + attribute,
                "failed to read a sysfs attribute"));
        }
        if (bytes == 0) {
            break;
        }
        if (value.size() + static_cast<std::size_t>(bytes) > kMaximumAttributeBytes) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::MalformedSysfs,
                LinuxUsbTopologyStage::AttributeRead,
                "sysfs attribute exceeds the bounded read limit",
                display_path + "/" + attribute));
        }
        value.append(buffer.data(), static_cast<std::size_t>(bytes));
    }
    if (!value.empty() && value.back() == '\n') {
        value.pop_back();
        if (!value.empty() && value.back() == '\r') {
            value.pop_back();
        }
    }
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos ||
        value.find('\0') != std::string::npos) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::MalformedSysfs,
            LinuxUsbTopologyStage::AttributeRead,
            "sysfs attribute contains embedded control delimiters",
            display_path + "/" + attribute));
    }
    return value;
}

struct RequiredIdentity final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> ports;

    [[nodiscard]] bool operator==(const RequiredIdentity&) const = default;
};

[[nodiscard]] std::expected<RequiredIdentity, LinuxUsbTopologyError>
read_required_identity(const int device, const std::string& display_path) {
    const auto bus = read_attribute(device, "busnum", display_path);
    if (!bus.has_value()) {
        return std::unexpected(bus.error());
    }
    const auto address = read_attribute(device, "devnum", display_path);
    if (!address.has_value()) {
        return std::unexpected(address.error());
    }
    const auto path = read_attribute(device, "devpath", display_path);
    if (!path.has_value()) {
        return std::unexpected(path.error());
    }
    const auto vendor = read_attribute(device, "idVendor", display_path);
    if (!vendor.has_value()) {
        return std::unexpected(vendor.error());
    }
    const auto product = read_attribute(device, "idProduct", display_path);
    if (!product.has_value()) {
        return std::unexpected(product.error());
    }

    const auto parsed_bus = parse_integer<std::uint8_t>(*bus, 10);
    const auto parsed_address = parse_integer<std::uint8_t>(*address, 10);
    const auto parsed_ports = parse_port_chain(*path);
    const auto parsed_vendor = parse_integer<std::uint16_t>(*vendor, 16);
    const auto parsed_product = parse_integer<std::uint16_t>(*product, 16);
    if (!parsed_bus.has_value() || *parsed_bus == 0U ||
        !parsed_address.has_value() || *parsed_address == 0U ||
        !parsed_ports.has_value() || !parsed_vendor.has_value() ||
        *parsed_vendor == 0U || !parsed_product.has_value() ||
        *parsed_product == 0U) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::MalformedSysfs,
            LinuxUsbTopologyStage::AttributeRead,
            "required USB sysfs identity attributes are malformed",
            display_path));
    }
    return RequiredIdentity{
        .vendor_id = *parsed_vendor,
        .product_id = *parsed_product,
        .bus_number = *parsed_bus,
        .device_address = *parsed_address,
        .ports = std::move(*parsed_ports),
    };
}

[[nodiscard]] std::expected<std::optional<std::string>, LinuxUsbTopologyError>
read_optional_identity(const int device,
                       const std::string_view name,
                       const std::string& display_path) {
    auto value = read_attribute(device, name, display_path);
    if (!value.has_value()) {
        if (value.error().kind == LinuxUsbTopologyErrorKind::NotFound) {
            return std::optional<std::string>{};
        }
        return std::unexpected(value.error());
    }
    if (value->empty()) {
        return std::optional<std::string>{};
    }
    if (!valid_identity_text(*value)) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::MalformedSysfs,
            LinuxUsbTopologyStage::AttributeRead,
            "optional USB identity attribute is not bounded valid UTF-8 text",
            display_path + "/" + std::string(name)));
    }
    return std::optional<std::string>{std::move(*value)};
}

[[nodiscard]] std::expected<std::string, LinuxUsbTopologyError>
controller_id(const std::vector<std::string>& components,
              const RequiredIdentity& identity,
              const std::string& entry,
              const std::string& display_path) {
    const auto root_hub = "usb" + std::to_string(identity.bus_number);
    const auto root = std::ranges::find(components, root_hub);
    if (root == components.end() || root == components.begin() + 1 ||
        std::ranges::find(root + 1, components.end(), root_hub) != components.end()) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::MalformedSysfs,
            LinuxUsbTopologyStage::Correlation,
            "resolved USB device path has no unique root-hub ancestor",
            display_path));
    }

    std::string prefix = std::to_string(identity.bus_number) + "-";
    auto hierarchy = root + 1;
    for (std::size_t index = 0; index < identity.ports.size(); ++index) {
        if (index != 0U) {
            prefix.push_back('.');
        }
        prefix += std::to_string(identity.ports[index]);
        if (hierarchy == components.end() || *hierarchy != prefix) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::MalformedSysfs,
                LinuxUsbTopologyStage::Correlation,
                "resolved USB path does not match its complete hub/port hierarchy",
                display_path));
        }
        ++hierarchy;
    }
    if (hierarchy != components.end() || components.back() != entry) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::MalformedSysfs,
            LinuxUsbTopologyStage::Correlation,
            "resolved USB path contains unexpected topology components",
            display_path));
    }

    const auto controller_components = std::span<const std::string>(
        components.data() + 1,
        static_cast<std::size_t>(root - components.begin() - 1));
    if (controller_components.empty()) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::MalformedSysfs,
            LinuxUsbTopologyStage::Correlation,
            "resolved USB root hub has no physical controller ancestor",
            display_path));
    }
    return "linux-sysfs:" + join_components(controller_components);
}

#endif  // !defined(_WIN32)

}  // namespace

OpenatLinuxUsbSysfsReader::OpenatLinuxUsbSysfsReader(
    std::string sysfs_root,
    const LinuxUsbSysfsCheckpointHook checkpoint_hook,
    void* const checkpoint_context)
    : sysfs_root_(std::move(sysfs_root)),
      checkpoint_hook_(checkpoint_hook),
      checkpoint_context_(checkpoint_context) {}

std::expected<std::vector<LinuxUsbSysfsNode>, LinuxUsbTopologyError>
OpenatLinuxUsbSysfsReader::read_candidates(
    const LinuxUsbTopologyQuery& query) const {
#if defined(_WIN32)
    (void)query;
    return std::unexpected(make_error(
        LinuxUsbTopologyErrorKind::UnsupportedPlatform,
        LinuxUsbTopologyStage::RootOpen,
        "Linux sysfs USB topology discovery is unavailable on Windows"));
#else
    try {
        if (!query_is_valid(query)) {
            return std::unexpected(make_error(
                query.port_numbers.size() > kMaximumUsbTopologyDepth
                    ? LinuxUsbTopologyErrorKind::TopologyTooDeep
                    : LinuxUsbTopologyErrorKind::InvalidArgument,
                LinuxUsbTopologyStage::Validation,
                "invalid libusb identity for Linux topology discovery"));
        }
        const auto entry = device_entry_name(query);
        if (!entry.has_value()) {
            return std::unexpected(entry.error());
        }

        auto root = open_absolute_root(sysfs_root_);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto opened_root_identity = directory_identity(
            root->get(), LinuxUsbTopologyStage::RootOpen, sysfs_root_);
        if (!opened_root_identity.has_value()) {
            return std::unexpected(opened_root_identity.error());
        }
        auto devices = open_usb_devices_directory(root->get());
        if (!devices.has_value()) {
            return std::unexpected(devices.error());
        }

        const auto display_path = "bus/usb/devices/" + *entry;
        const auto changed_from = [&display_path](
                                      const LinuxUsbTopologyError& cause,
                                      std::string message) {
            return make_error(
                LinuxUsbTopologyErrorKind::IdentityChanged,
                LinuxUsbTopologyStage::Correlation,
                std::move(message),
                cause.path.empty() ? display_path : cause.path,
                cause.native_code);
        };
        const auto target = read_link_at(devices->get(), *entry, display_path);
        if (!target.has_value()) {
            return std::unexpected(target.error());
        }
        const auto components = normalize_link_target(*target, *entry, display_path);
        if (!components.has_value()) {
            return std::unexpected(components.error());
        }
        auto device = open_resolved_device(root->get(), *components, display_path);
        if (!device.has_value()) {
            return std::unexpected(device.error());
        }
        const auto opened_device_identity = directory_identity(
            device->get(), LinuxUsbTopologyStage::SymlinkResolution, display_path);
        if (!opened_device_identity.has_value()) {
            return std::unexpected(opened_device_identity.error());
        }
        if (checkpoint_hook_ != nullptr) {
            checkpoint_hook_(LinuxUsbSysfsReadCheckpoint::DeviceOpened,
                             checkpoint_context_);
        }

        const auto before = read_required_identity(device->get(), display_path);
        if (!before.has_value()) {
            return std::unexpected(before.error());
        }
        const auto serial = read_optional_identity(device->get(), "serial", display_path);
        if (!serial.has_value()) {
            return std::unexpected(serial.error());
        }
        const auto product = read_optional_identity(device->get(), "product", display_path);
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        const auto after = read_required_identity(device->get(), display_path);
        if (!after.has_value()) {
            return std::unexpected(after.error());
        }
        if (*before != *after) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::IdentityChanged,
                LinuxUsbTopologyStage::Correlation,
                "USB sysfs identity changed while it was being read",
                display_path));
        }
        if (checkpoint_hook_ != nullptr) {
            checkpoint_hook_(LinuxUsbSysfsReadCheckpoint::SnapshotRead,
                             checkpoint_context_);
        }

        // Re-open from the configured absolute root and verify both inode
        // identities and the bus symlink text. This detects root replacement,
        // link retargeting, and same-target leaf replacement after the held
        // descriptors were opened.
        auto stable_root = open_absolute_root(sysfs_root_);
        if (!stable_root.has_value()) {
            return std::unexpected(changed_from(
                stable_root.error(),
                "sysfs root changed while USB topology was being read"));
        }
        const auto stable_root_identity = directory_identity(
            stable_root->get(), LinuxUsbTopologyStage::RootOpen, sysfs_root_);
        if (!stable_root_identity.has_value()) {
            return std::unexpected(changed_from(
                stable_root_identity.error(),
                "sysfs root changed while USB topology was being read"));
        }
        if (*opened_root_identity != *stable_root_identity) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::IdentityChanged,
                LinuxUsbTopologyStage::Correlation,
                "sysfs root identity changed while USB topology was being read",
                sysfs_root_));
        }
        auto stable_devices = open_usb_devices_directory(stable_root->get());
        if (!stable_devices.has_value()) {
            return std::unexpected(changed_from(
                stable_devices.error(),
                "USB sysfs lookup changed while topology was being read"));
        }
        const auto stable_target = read_link_at(
            stable_devices->get(), *entry, display_path);
        if (!stable_target.has_value()) {
            return std::unexpected(changed_from(
                stable_target.error(),
                "USB sysfs link changed while topology was being read"));
        }
        if (*stable_target != *target) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::IdentityChanged,
                LinuxUsbTopologyStage::Correlation,
                "USB sysfs link target changed while topology was being read",
                display_path));
        }
        auto stable_device = open_resolved_device(
            stable_root->get(), *components, display_path);
        if (!stable_device.has_value()) {
            return std::unexpected(changed_from(
                stable_device.error(),
                "USB sysfs device path changed while topology was being read"));
        }
        const auto stable_device_identity = directory_identity(
            stable_device->get(),
            LinuxUsbTopologyStage::SymlinkResolution,
            display_path);
        if (!stable_device_identity.has_value()) {
            return std::unexpected(changed_from(
                stable_device_identity.error(),
                "USB sysfs device identity changed while topology was being read"));
        }
        if (*opened_device_identity != *stable_device_identity) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::IdentityChanged,
                LinuxUsbTopologyStage::Correlation,
                "USB sysfs device inode changed while topology was being read",
                display_path));
        }

        const auto stable_before = read_required_identity(
            stable_device->get(), display_path);
        if (!stable_before.has_value()) {
            return std::unexpected(changed_from(
                stable_before.error(),
                "USB sysfs identity changed during final validation"));
        }
        const auto stable_serial = read_optional_identity(
            stable_device->get(), "serial", display_path);
        if (!stable_serial.has_value()) {
            return std::unexpected(changed_from(
                stable_serial.error(),
                "USB sysfs serial changed during final validation"));
        }
        const auto stable_product = read_optional_identity(
            stable_device->get(), "product", display_path);
        if (!stable_product.has_value()) {
            return std::unexpected(changed_from(
                stable_product.error(),
                "USB sysfs product changed during final validation"));
        }
        const auto stable_after = read_required_identity(
            stable_device->get(), display_path);
        if (!stable_after.has_value()) {
            return std::unexpected(changed_from(
                stable_after.error(),
                "USB sysfs identity changed during final validation"));
        }
        if (*stable_before != *stable_after || *before != *stable_before ||
            *serial != *stable_serial || *product != *stable_product) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::IdentityChanged,
                LinuxUsbTopologyStage::Correlation,
                "USB sysfs identity changed during final validation",
                display_path));
        }

        const auto controller = controller_id(*components, *before, *entry, display_path);
        if (!controller.has_value()) {
            return std::unexpected(controller.error());
        }

        std::vector<LinuxUsbSysfsNode> result;
        result.push_back(LinuxUsbSysfsNode{
            .entry_name = *entry,
            .root_relative_path = join_components(*components),
            .root_controller_id = *controller,
            .vendor_id = before->vendor_id,
            .product_id = before->product_id,
            .bus_number = before->bus_number,
            .device_address = before->device_address,
            .port_numbers = before->ports,
            .serial_utf8 = *serial,
            .product_utf8 = *product,
        });
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::ResourceExhausted,
            LinuxUsbTopologyStage::Lookup,
            "memory allocation failed during Linux USB topology discovery"));
    }
#endif
}

LinuxUsbTopologyDiscovery::LinuxUsbTopologyDiscovery(
    const ILinuxUsbSysfsReader& reader) noexcept
    : reader_(reader) {}

std::expected<LinuxUsbTopology, LinuxUsbTopologyError>
LinuxUsbTopologyDiscovery::discover(const LinuxUsbTopologyQuery& query) const {
    try {
        if (!query_is_valid(query)) {
            return std::unexpected(make_error(
                query.port_numbers.size() > kMaximumUsbTopologyDepth
                    ? LinuxUsbTopologyErrorKind::TopologyTooDeep
                    : LinuxUsbTopologyErrorKind::InvalidArgument,
                LinuxUsbTopologyStage::Validation,
                "invalid libusb identity for Linux topology discovery"));
        }
        auto candidates = reader_.read_candidates(query);
        if (!candidates.has_value()) {
            return std::unexpected(candidates.error());
        }

        const auto expected_entry = device_entry_name(query);
        if (!expected_entry.has_value()) {
            return std::unexpected(expected_entry.error());
        }
        const LinuxUsbSysfsNode* match = nullptr;
        bool identity_mismatch = false;
        for (const auto& candidate : *candidates) {
            if (!node_shape_is_valid(candidate)) {
                return std::unexpected(make_error(
                    LinuxUsbTopologyErrorKind::MalformedSysfs,
                    LinuxUsbTopologyStage::Correlation,
                    "sysfs reader returned a malformed USB node",
                    candidate.root_relative_path));
            }
            if (candidate.entry_name != *expected_entry ||
                candidate.bus_number != query.bus_number ||
                candidate.device_address != query.device_address ||
                candidate.port_numbers != query.port_numbers ||
                candidate.vendor_id != query.vendor_id ||
                candidate.product_id != query.product_id) {
                identity_mismatch = true;
                continue;
            }
            if ((query.serial_utf8.has_value() &&
                 (!candidate.serial_utf8.has_value() ||
                  *query.serial_utf8 != *candidate.serial_utf8)) ||
                (query.product_utf8.has_value() &&
                 (!candidate.product_utf8.has_value() ||
                  *query.product_utf8 != *candidate.product_utf8))) {
                identity_mismatch = true;
                continue;
            }
            if (match != nullptr) {
                return std::unexpected(make_error(
                    LinuxUsbTopologyErrorKind::AmbiguousMapping,
                    LinuxUsbTopologyStage::Correlation,
                    "more than one sysfs node maps to the same libusb device",
                    candidate.root_relative_path));
            }
            match = &candidate;
        }
        if (match == nullptr) {
            return std::unexpected(make_error(
                identity_mismatch ? LinuxUsbTopologyErrorKind::IdentityMismatch
                                  : LinuxUsbTopologyErrorKind::NotFound,
                LinuxUsbTopologyStage::Correlation,
                identity_mismatch
                    ? "sysfs USB identity does not match the libusb snapshot"
                    : "no sysfs node maps to the libusb device"));
        }

        auto physical_path = canonical_linux_usb_port_path(
            match->bus_number, match->port_numbers);
        if (!physical_path.has_value()) {
            return std::unexpected(physical_path.error());
        }
        return LinuxUsbTopology{
            .physical_port_path = std::move(*physical_path),
            .root_controller_id = match->root_controller_id,
            .hub_port_chain = match->port_numbers,
            .vendor_id = match->vendor_id,
            .product_id = match->product_id,
            .bus_number = match->bus_number,
            .device_address = match->device_address,
            .serial_utf8 = match->serial_utf8,
            .product_utf8 = match->product_utf8,
            .sysfs_device_path = match->root_relative_path,
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::ResourceExhausted,
            LinuxUsbTopologyStage::Correlation,
            "memory allocation failed while correlating USB topology"));
    }
}

LinuxUsbTopologyQuery make_linux_usb_topology_query(
    const UsbDeviceInfo& device,
    const std::optional<std::string_view> product_utf8) {
    return LinuxUsbTopologyQuery{
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .port_numbers = device.port_path,
        .serial_utf8 = device.serial_utf8.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{device.serial_utf8},
        .product_utf8 = product_utf8.has_value() && !product_utf8->empty()
            ? std::optional<std::string>{*product_utf8}
            : std::optional<std::string>{},
    };
}

std::expected<std::string, LinuxUsbTopologyError>
canonical_linux_usb_port_path(
    const std::uint8_t bus_number,
    const std::vector<std::uint8_t>& port_numbers) {
    try {
        if (bus_number == 0U || port_numbers.empty()) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::InvalidArgument,
                LinuxUsbTopologyStage::Validation,
                "USB physical path requires a non-zero bus and port chain"));
        }
        if (port_numbers.size() > kMaximumUsbTopologyDepth) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::TopologyTooDeep,
                LinuxUsbTopologyStage::Validation,
                "USB hub depth exceeds the supported libusb path limit"));
        }
        if (std::ranges::any_of(port_numbers, [](const std::uint8_t port) {
                return port == 0U;
            })) {
            return std::unexpected(make_error(
                LinuxUsbTopologyErrorKind::InvalidArgument,
                LinuxUsbTopologyStage::Validation,
                "USB physical path contains a zero port number"));
        }

        std::string result = "usb:" + std::to_string(bus_number) + "-";
        for (std::size_t index = 0; index < port_numbers.size(); ++index) {
            if (index != 0U) {
                result.push_back('.');
            }
            result += std::to_string(port_numbers[index]);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            LinuxUsbTopologyErrorKind::ResourceExhausted,
            LinuxUsbTopologyStage::Validation,
            "memory allocation failed while formatting USB physical path"));
    }
}

}  // namespace kairosboot::transport
