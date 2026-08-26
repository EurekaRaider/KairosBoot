// SPDX-License-Identifier: MIT
#include "src/transport/linux_usb_topology.hpp"

#include "src/transport/libusb_runtime.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::transport::ILinuxUsbSysfsReader;
using kairosboot::transport::LinuxUsbSysfsNode;
using kairosboot::transport::LinuxUsbTopology;
using kairosboot::transport::LinuxUsbTopologyDiscovery;
using kairosboot::transport::LinuxUsbTopologyError;
using kairosboot::transport::LinuxUsbTopologyErrorKind;
using kairosboot::transport::LinuxUsbTopologyQuery;
using kairosboot::transport::LinuxUsbSysfsReadCheckpoint;
using kairosboot::transport::OpenatLinuxUsbSysfsReader;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::canonical_linux_usb_port_path;
using kairosboot::transport::make_linux_usb_topology_query;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

[[nodiscard]] LinuxUsbTopologyQuery query(
    const std::uint8_t bus = 1U,
    const std::uint8_t address = 5U,
    std::vector<std::uint8_t> ports = {2U, 3U}) {
    return LinuxUsbTopologyQuery{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = bus,
        .device_address = address,
        .port_numbers = std::move(ports),
        .serial_utf8 = std::string{"SERIAL-01"},
        .product_utf8 = std::string{"Kairos device"},
    };
}

[[nodiscard]] std::string entry_name(const LinuxUsbTopologyQuery& value) {
    auto path = canonical_linux_usb_port_path(value.bus_number, value.port_numbers);
    CHECK(path.has_value());
    return path->substr(4U);
}

[[nodiscard]] LinuxUsbSysfsNode node(
    const LinuxUsbTopologyQuery& value,
    std::string controller = "linux-sysfs:pci0000:00/0000:00:14.0") {
    const auto entry = entry_name(value);
    const auto controller_path =
        std::string_view(controller).substr(std::string_view{"linux-sysfs:"}.size());
    std::string root_relative =
        "devices/" + std::string(controller_path) + "/usb" +
        std::to_string(value.bus_number);
    std::string prefix = std::to_string(value.bus_number) + "-";
    for (std::size_t index = 0; index < value.port_numbers.size(); ++index) {
        if (index != 0U) {
            prefix.push_back('.');
        }
        prefix += std::to_string(value.port_numbers[index]);
        root_relative.push_back('/');
        root_relative += prefix;
    }
    return LinuxUsbSysfsNode{
        .entry_name = entry,
        .root_relative_path = std::move(root_relative),
        .root_controller_id = std::move(controller),
        .vendor_id = value.vendor_id,
        .product_id = value.product_id,
        .bus_number = value.bus_number,
        .device_address = value.device_address,
        .port_numbers = value.port_numbers,
        .serial_utf8 = value.serial_utf8,
        .product_utf8 = value.product_utf8,
    };
}

class FakeReader final : public ILinuxUsbSysfsReader {
public:
    std::expected<std::vector<LinuxUsbSysfsNode>, LinuxUsbTopologyError> result{
        std::vector<LinuxUsbSysfsNode>{}};
    mutable std::size_t calls{};

    [[nodiscard]] std::expected<std::vector<LinuxUsbSysfsNode>,
                                LinuxUsbTopologyError>
    read_candidates(const LinuxUsbTopologyQuery&) const override {
        ++calls;
        return result;
    }
};

void canonical_identity_maps_to_existing_consumers() {
    const auto wanted = query();
    FakeReader reader;
    reader.result = std::vector{node(wanted)};
    LinuxUsbTopologyDiscovery discovery(reader);

    const auto topology = discovery.discover(wanted);
    CHECK(topology.has_value());
    CHECK(topology->physical_port_path == "usb:1-2.3");
    CHECK(topology->root_controller_id ==
          "linux-sysfs:pci0000:00/0000:00:14.0");
    CHECK(topology->hub_port_chain == std::vector<std::uint8_t>({2U, 3U}));
    CHECK(topology->bus_number == 1U);
    CHECK(topology->device_address == 5U);
    CHECK(topology->serial_utf8 == wanted.serial_utf8);
    CHECK(topology->product_utf8 == wanted.product_utf8);
}

void transient_address_does_not_change_physical_identity() {
    auto before = query(1U, 5U, {2U, 3U});
    auto after = before;
    after.device_address = 71U;

    FakeReader first_reader;
    first_reader.result = std::vector{node(before)};
    FakeReader second_reader;
    second_reader.result = std::vector{node(after)};
    LinuxUsbTopologyDiscovery first(first_reader);
    LinuxUsbTopologyDiscovery second(second_reader);

    const auto first_topology = first.discover(before);
    const auto second_topology = second.discover(after);
    CHECK(first_topology.has_value());
    CHECK(second_topology.has_value());
    CHECK(first_topology->physical_port_path == second_topology->physical_port_path);
    CHECK(first_topology->root_controller_id == second_topology->root_controller_id);
    CHECK(first_topology->device_address != second_topology->device_address);
}

void duplicate_and_mismatched_mappings_fail_closed() {
    const auto wanted = query();
    const auto mapped = node(wanted);

    FakeReader duplicate;
    duplicate.result = std::vector{mapped, mapped};
    LinuxUsbTopologyDiscovery duplicate_discovery(duplicate);
    const auto ambiguous = duplicate_discovery.discover(wanted);
    CHECK(!ambiguous.has_value());
    CHECK(ambiguous.error().kind == LinuxUsbTopologyErrorKind::AmbiguousMapping);

    auto changed = mapped;
    changed.serial_utf8 = "OTHER";
    FakeReader mismatch;
    mismatch.result = std::vector{changed};
    LinuxUsbTopologyDiscovery mismatch_discovery(mismatch);
    const auto rejected = mismatch_discovery.discover(wanted);
    CHECK(!rejected.has_value());
    CHECK(rejected.error().kind == LinuxUsbTopologyErrorKind::IdentityMismatch);

    auto traversal = mapped;
    traversal.root_relative_path = "devices/controller/../escape/1-2.3";
    FakeReader malformed;
    malformed.result = std::vector{std::move(traversal)};
    LinuxUsbTopologyDiscovery malformed_discovery(malformed);
    const auto malformed_result = malformed_discovery.discover(wanted);
    CHECK(!malformed_result.has_value());
    CHECK(malformed_result.error().kind == LinuxUsbTopologyErrorKind::MalformedSysfs);

    auto crossed_controller = mapped;
    crossed_controller.root_controller_id =
        "linux-sysfs:pci0000:00/0000:00:15.0";
    FakeReader crossed;
    crossed.result = std::vector{std::move(crossed_controller)};
    LinuxUsbTopologyDiscovery crossed_discovery(crossed);
    const auto crossed_result = crossed_discovery.discover(wanted);
    CHECK(!crossed_result.has_value());
    CHECK(crossed_result.error().kind ==
          LinuxUsbTopologyErrorKind::MalformedSysfs);
}

void missing_expected_identity_fails_closed() {
    const auto wanted = query();
    auto mapped = node(wanted);
    mapped.serial_utf8.reset();
    mapped.product_utf8.reset();
    FakeReader reader;
    reader.result = std::vector{std::move(mapped)};
    LinuxUsbTopologyDiscovery discovery(reader);

    const auto rejected = discovery.discover(wanted);
    CHECK(!rejected.has_value());
    CHECK(rejected.error().kind == LinuxUsbTopologyErrorKind::IdentityMismatch);

    auto anonymous = wanted;
    anonymous.serial_utf8.reset();
    anonymous.product_utf8.reset();
    FakeReader anonymous_reader;
    anonymous_reader.result = std::vector{node(anonymous)};
    LinuxUsbTopologyDiscovery anonymous_discovery(anonymous_reader);
    const auto accepted = anonymous_discovery.discover(anonymous);
    CHECK(accepted.has_value());
    CHECK(!accepted->serial_utf8.has_value());
    CHECK(!accepted->product_utf8.has_value());
}

void invalid_and_overdeep_topologies_never_reach_the_reader() {
    FakeReader reader;
    LinuxUsbTopologyDiscovery discovery(reader);

    auto invalid = query();
    invalid.port_numbers = {2U, 0U};
    const auto invalid_result = discovery.discover(invalid);
    CHECK(!invalid_result.has_value());
    CHECK(invalid_result.error().kind == LinuxUsbTopologyErrorKind::InvalidArgument);

    auto deep = query();
    deep.port_numbers.assign(17U, 1U);
    const auto deep_result = discovery.discover(deep);
    CHECK(!deep_result.has_value());
    CHECK(deep_result.error().kind == LinuxUsbTopologyErrorKind::TopologyTooDeep);
    CHECK(reader.calls == 0U);
}

void permission_and_missing_errors_are_preserved() {
    const auto wanted = query();
    FakeReader permission;
    permission.result = std::unexpected(LinuxUsbTopologyError{
        .kind = LinuxUsbTopologyErrorKind::PermissionDenied,
        .stage = kairosboot::transport::LinuxUsbTopologyStage::AttributeRead,
        .native_code = 13,
        .path = "bus/usb/devices/1-2.3/idVendor",
        .message = "permission denied",
    });
    LinuxUsbTopologyDiscovery permission_discovery(permission);
    const auto denied = permission_discovery.discover(wanted);
    CHECK(!denied.has_value());
    CHECK(denied.error().kind == LinuxUsbTopologyErrorKind::PermissionDenied);
    CHECK(denied.error().native_code == 13);

    FakeReader missing;
    LinuxUsbTopologyDiscovery missing_discovery(missing);
    const auto absent = missing_discovery.discover(wanted);
    CHECK(!absent.has_value());
    CHECK(absent.error().kind == LinuxUsbTopologyErrorKind::NotFound);

    FakeReader changed;
    changed.result = std::unexpected(LinuxUsbTopologyError{
        .kind = LinuxUsbTopologyErrorKind::IdentityChanged,
        .stage = kairosboot::transport::LinuxUsbTopologyStage::Correlation,
        .native_code = 0,
        .path = "bus/usb/devices/1-2.3",
        .message = "identity changed during snapshot",
    });
    LinuxUsbTopologyDiscovery changed_discovery(changed);
    const auto raced = changed_discovery.discover(wanted);
    CHECK(!raced.has_value());
    CHECK(raced.error().kind == LinuxUsbTopologyErrorKind::IdentityChanged);
}

void libusb_snapshot_adapter_is_lossless() {
    UsbDeviceInfo device{};
    device.vendor_id = 0x18D1U;
    device.product_id = 0x4EE0U;
    device.bus_number = 4U;
    device.device_address = 9U;
    device.port_path = {1U, 8U};
    device.serial_utf8 = "SERIAL-ADAPTER";

    const auto adapted = make_linux_usb_topology_query(device, "Product descriptor");
    CHECK(adapted.vendor_id == device.vendor_id);
    CHECK(adapted.product_id == device.product_id);
    CHECK(adapted.bus_number == device.bus_number);
    CHECK(adapted.device_address == device.device_address);
    CHECK(adapted.port_numbers == device.port_path);
    CHECK(adapted.serial_utf8 == std::optional<std::string>{device.serial_utf8});
    CHECK(adapted.product_utf8 == std::optional<std::string>{"Product descriptor"});

    auto second_interface = device;
    second_interface.interface_number = 9U;
    second_interface.alternate_setting = 2U;
    second_interface.interface_class = 0xFEU;
    second_interface.bulk_out_endpoint = 0x07U;
    const auto second_query =
        make_linux_usb_topology_query(second_interface, "Product descriptor");
    CHECK(second_query.vendor_id == adapted.vendor_id);
    CHECK(second_query.product_id == adapted.product_id);
    CHECK(second_query.bus_number == adapted.bus_number);
    CHECK(second_query.device_address == adapted.device_address);
    CHECK(second_query.port_numbers == adapted.port_numbers);
    CHECK(second_query.serial_utf8 == adapted.serial_utf8);
    CHECK(second_query.product_utf8 == adapted.product_utf8);
}

#if !defined(_WIN32)

class TempSysfs final {
public:
    TempSysfs() {
        const auto trusted_temp = std::filesystem::canonical(
            std::filesystem::temp_directory_path());
        const auto seed = std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count();
        for (std::uint32_t attempt = 0; attempt < 128U; ++attempt) {
            auto candidate = trusted_temp /
                ("kairosboot-linux-topology-" + std::to_string(seed) + "-" +
                 std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root_ = std::move(candidate);
                break;
            }
            if (error) {
                throw std::runtime_error("failed to create hermetic sysfs root");
            }
        }
        if (root_.empty()) {
            throw std::runtime_error("could not allocate a unique hermetic sysfs root");
        }
        std::filesystem::create_directories(root_ / "bus/usb/devices");
        std::filesystem::create_directories(root_ / "devices");
    }

    ~TempSysfs() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempSysfs(const TempSysfs&) = delete;
    TempSysfs& operator=(const TempSysfs&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    [[nodiscard]] std::filesystem::path add_device(
        const LinuxUsbTopologyQuery& value,
        const std::string_view controller = "pci0000:00/0000:00:14.0",
        const bool publish_bus_link = true) const {
        const auto entry = entry_name(value);
        auto device_path = root_ / "devices" / std::string(controller) /
            ("usb" + std::to_string(value.bus_number));
        std::string prefix = std::to_string(value.bus_number) + "-";
        for (std::size_t index = 0; index < value.port_numbers.size(); ++index) {
            if (index != 0U) {
                prefix.push_back('.');
            }
            prefix += std::to_string(value.port_numbers[index]);
            device_path /= prefix;
        }
        std::filesystem::create_directories(device_path);
        write(device_path / "busnum", std::to_string(value.bus_number) + "\n");
        write(device_path / "devnum", std::to_string(value.device_address) + "\n");

        std::string devpath;
        for (std::size_t index = 0; index < value.port_numbers.size(); ++index) {
            if (index != 0U) {
                devpath.push_back('.');
            }
            devpath += std::to_string(value.port_numbers[index]);
        }
        write(device_path / "devpath", devpath + "\n");

        std::array<char, 5U> hex{};
        const auto format_hex = [&hex](const std::uint16_t number) {
            constexpr std::string_view digits = "0123456789abcdef";
            for (std::size_t index = 0; index < 4U; ++index) {
                const auto shift = static_cast<unsigned int>((3U - index) * 4U);
                hex[index] = digits[(number >> shift) & 0xFU];
            }
            hex[4] = '\0';
            return std::string(hex.data(), 4U);
        };
        write(device_path / "idVendor", format_hex(value.vendor_id) + "\n");
        write(device_path / "idProduct", format_hex(value.product_id) + "\n");
        if (value.serial_utf8.has_value()) {
            write(device_path / "serial", *value.serial_utf8 + "\n");
        }
        if (value.product_utf8.has_value()) {
            write(device_path / "product", *value.product_utf8 + "\n");
        }

        if (publish_bus_link) {
            const auto root_relative =
                std::filesystem::relative(device_path, root_).generic_string();
            std::filesystem::create_symlink(
                "../../../" + root_relative,
                root_ / "bus/usb/devices" / entry);
        }
        return device_path;
    }

    static void write(const std::filesystem::path& path, const std::string_view value) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create hermetic sysfs attribute");
        }
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        if (!output) {
            throw std::runtime_error("failed to write hermetic sysfs attribute");
        }
    }

private:
    std::filesystem::path root_;
};

struct LinkRetargetContext final {
    std::filesystem::path link;
    std::string replacement_target;
    std::error_code error;
    bool invoked{};
};

void retarget_link_after_open(
    const LinuxUsbSysfsReadCheckpoint checkpoint,
    void* const opaque) noexcept {
    if (checkpoint != LinuxUsbSysfsReadCheckpoint::DeviceOpened) {
        return;
    }
    auto& context = *static_cast<LinkRetargetContext*>(opaque);
    context.invoked = true;
    const auto removed = std::filesystem::remove(context.link, context.error);
    if (!removed || context.error) {
        return;
    }
    std::filesystem::create_symlink(
        context.replacement_target, context.link, context.error);
}

struct DirectoryReplacementContext final {
    std::filesystem::path current;
    std::filesystem::path replacement;
    std::filesystem::path displaced;
    std::error_code error;
    bool invoked{};
};

void replace_directory_after_snapshot(
    const LinuxUsbSysfsReadCheckpoint checkpoint,
    void* const opaque) noexcept {
    if (checkpoint != LinuxUsbSysfsReadCheckpoint::SnapshotRead) {
        return;
    }
    auto& context = *static_cast<DirectoryReplacementContext*>(opaque);
    context.invoked = true;
    std::filesystem::rename(context.current, context.displaced, context.error);
    if (context.error) {
        return;
    }
    std::filesystem::rename(
        context.replacement, context.current, context.error);
}

[[nodiscard]] LinuxUsbTopology read_real_tree(
    const TempSysfs& tree,
    const LinuxUsbTopologyQuery& wanted) {
    OpenatLinuxUsbSysfsReader reader(tree.root().string());
    LinuxUsbTopologyDiscovery discovery(reader);
    const auto result = discovery.discover(wanted);
    CHECK(result.has_value());
    return *result;
}

void openat_reader_correlates_a_hermetic_sysfs_tree() {
    TempSysfs tree;
    const auto wanted = query();
    (void)tree.add_device(wanted);

    const auto topology = read_real_tree(tree, wanted);
    CHECK(topology.physical_port_path == "usb:1-2.3");
    CHECK(topology.root_controller_id ==
          "linux-sysfs:pci0000:00/0000:00:14.0");
    CHECK(topology.sysfs_device_path ==
          "devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2.3");
}

void usb2_and_usb3_companions_share_the_physical_controller() {
    TempSysfs tree;
    auto usb2 = query(1U, 5U, {4U});
    auto usb3 = query(2U, 8U, {4U});
    usb3.serial_utf8 = "SERIAL-USB3";
    (void)tree.add_device(usb2);
    (void)tree.add_device(usb3);

    const auto usb2_topology = read_real_tree(tree, usb2);
    const auto usb3_topology = read_real_tree(tree, usb3);
    CHECK(usb2_topology.root_controller_id == usb3_topology.root_controller_id);
    CHECK(usb2_topology.physical_port_path == "usb:1-4");
    CHECK(usb3_topology.physical_port_path == "usb:2-4");
}

void reenumeration_address_change_keeps_port_and_controller_stable() {
    TempSysfs tree;
    auto before = query(1U, 5U, {2U, 3U});
    const auto device_path = tree.add_device(before);
    const auto first = read_real_tree(tree, before);

    auto after = before;
    after.device_address = 29U;
    TempSysfs::write(device_path / "devnum", "29\n");
    const auto second = read_real_tree(tree, after);
    CHECK(first.physical_port_path == second.physical_port_path);
    CHECK(first.root_controller_id == second.root_controller_id);
    CHECK(first.device_address == 5U);
    CHECK(second.device_address == 29U);
}

void missing_and_malformed_sysfs_fail_with_specific_errors() {
    TempSysfs missing_tree;
    const auto wanted = query();
    OpenatLinuxUsbSysfsReader missing_reader(missing_tree.root().string());
    const auto missing = missing_reader.read_candidates(wanted);
    CHECK(!missing.has_value());
    CHECK(missing.error().kind == LinuxUsbTopologyErrorKind::NotFound);

    TempSysfs malformed_tree;
    const auto device_path = malformed_tree.add_device(wanted);
    TempSysfs::write(device_path / "idVendor", "not-hex\n");
    OpenatLinuxUsbSysfsReader malformed_reader(malformed_tree.root().string());
    const auto malformed = malformed_reader.read_candidates(wanted);
    CHECK(!malformed.has_value());
    CHECK(malformed.error().kind == LinuxUsbTopologyErrorKind::MalformedSysfs);
}

void link_escape_and_attribute_symlink_are_never_followed() {
    const auto wanted = query();
    TempSysfs escape_tree;
    std::filesystem::create_symlink(
        "../../../../outside-the-configured-root",
        escape_tree.root() / "bus/usb/devices" / entry_name(wanted));
    OpenatLinuxUsbSysfsReader escape_reader(escape_tree.root().string());
    const auto escaped = escape_reader.read_candidates(wanted);
    CHECK(!escaped.has_value());
    CHECK(escaped.error().kind == LinuxUsbTopologyErrorKind::UnsafePath);

    TempSysfs attribute_tree;
    const auto device_path = attribute_tree.add_device(wanted);
    std::filesystem::remove(device_path / "idVendor");
    TempSysfs::write(attribute_tree.root() / "secret", "18d1\n");
    std::filesystem::create_symlink(attribute_tree.root() / "secret",
                                    device_path / "idVendor");
    OpenatLinuxUsbSysfsReader attribute_reader(attribute_tree.root().string());
    const auto linked_attribute = attribute_reader.read_candidates(wanted);
    CHECK(!linked_attribute.has_value());
    CHECK(linked_attribute.error().kind == LinuxUsbTopologyErrorKind::UnsafePath);
}

void target_path_symlinks_and_symlinked_roots_are_rejected() {
    const auto wanted = query(1U, 5U, {2U});
    TempSysfs target_tree;
    std::filesystem::create_directories(target_tree.root() / "outside/usb1/1-2");
    std::filesystem::create_symlink(target_tree.root() / "outside",
                                    target_tree.root() / "devices/controller");
    std::filesystem::create_symlink(
        "../../../devices/controller/usb1/1-2",
        target_tree.root() / "bus/usb/devices/1-2");
    OpenatLinuxUsbSysfsReader target_reader(target_tree.root().string());
    const auto target_result = target_reader.read_candidates(wanted);
    CHECK(!target_result.has_value());
    CHECK(target_result.error().kind == LinuxUsbTopologyErrorKind::UnsafePath);

    TempSysfs real_tree;
    (void)real_tree.add_device(wanted);
    const auto root_link = real_tree.root().parent_path() /
        (real_tree.root().filename().string() + "-link");
    std::filesystem::create_symlink(real_tree.root(), root_link);
    OpenatLinuxUsbSysfsReader root_reader(root_link.string());
    const auto root_result = root_reader.read_candidates(wanted);
    std::filesystem::remove(root_link);
    CHECK(!root_result.has_value());
    CHECK(root_result.error().kind == LinuxUsbTopologyErrorKind::UnsafePath);
}

void link_retarget_and_same_target_replacement_fail_closed() {
    const auto wanted = query();

    TempSysfs retarget_tree;
    (void)retarget_tree.add_device(wanted);
    const auto alternate = retarget_tree.add_device(
        wanted, "pci0000:00/0000:00:15.0", false);
    LinkRetargetContext retarget{
        .link = retarget_tree.root() / "bus/usb/devices" / entry_name(wanted),
        .replacement_target =
            "../../../" +
            std::filesystem::relative(alternate, retarget_tree.root())
                .generic_string(),
    };
    OpenatLinuxUsbSysfsReader retarget_reader(
        retarget_tree.root().string(), retarget_link_after_open, &retarget);
    const auto retargeted = retarget_reader.read_candidates(wanted);
    CHECK(retarget.invoked);
    CHECK(!retarget.error);
    CHECK(!retargeted.has_value());
    CHECK(retargeted.error().kind == LinuxUsbTopologyErrorKind::IdentityChanged);

    TempSysfs replacement_tree;
    const auto current = replacement_tree.add_device(wanted);
    const auto replacement = replacement_tree.root() / "replacement-device";
    std::error_code copy_error;
    std::filesystem::copy(
        current,
        replacement,
        std::filesystem::copy_options::recursive,
        copy_error);
    CHECK(!copy_error);
    DirectoryReplacementContext leaf{
        .current = current,
        .replacement = replacement,
        .displaced = current.parent_path() /
            (current.filename().string() + "-displaced"),
    };
    OpenatLinuxUsbSysfsReader replacement_reader(
        replacement_tree.root().string(), replace_directory_after_snapshot, &leaf);
    const auto replaced = replacement_reader.read_candidates(wanted);
    CHECK(leaf.invoked);
    CHECK(!leaf.error);
    CHECK(!replaced.has_value());
    CHECK(replaced.error().kind == LinuxUsbTopologyErrorKind::IdentityChanged);

    TempSysfs root_tree;
    (void)root_tree.add_device(wanted);
    TempSysfs new_root_tree;
    (void)new_root_tree.add_device(wanted);
    const auto displaced_root = root_tree.root().parent_path() /
        (root_tree.root().filename().string() + "-displaced-root");
    DirectoryReplacementContext root_replacement{
        .current = root_tree.root(),
        .replacement = new_root_tree.root(),
        .displaced = displaced_root,
    };
    OpenatLinuxUsbSysfsReader root_reader(
        root_tree.root().string(),
        replace_directory_after_snapshot,
        &root_replacement);
    const auto replaced_root = root_reader.read_candidates(wanted);
    std::error_code cleanup_error;
    std::filesystem::remove_all(displaced_root, cleanup_error);
    CHECK(root_replacement.invoked);
    CHECK(!root_replacement.error);
    CHECK(!cleanup_error);
    CHECK(!replaced_root.has_value());
    CHECK(replaced_root.error().kind ==
          LinuxUsbTopologyErrorKind::IdentityChanged);
}

#endif  // !defined(_WIN32)

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"consumer identity mapping", canonical_identity_maps_to_existing_consumers},
        {"transient address", transient_address_does_not_change_physical_identity},
        {"ambiguous and mismatched", duplicate_and_mismatched_mappings_fail_closed},
        {"missing expected identity", missing_expected_identity_fails_closed},
        {"invalid topology", invalid_and_overdeep_topologies_never_reach_the_reader},
        {"reader error preservation", permission_and_missing_errors_are_preserved},
        {"libusb adapter", libusb_snapshot_adapter_is_lossless},
#if !defined(_WIN32)
        {"hermetic openat reader", openat_reader_correlates_a_hermetic_sysfs_tree},
        {"USB companion controller", usb2_and_usb3_companions_share_the_physical_controller},
        {"real-tree reenumeration", reenumeration_address_change_keeps_port_and_controller_stable},
        {"missing and malformed sysfs", missing_and_malformed_sysfs_fail_with_specific_errors},
        {"link containment", link_escape_and_attribute_symlink_are_never_followed},
        {"path containment", target_path_symlinks_and_symlinked_roots_are_rejected},
        {"sysfs replacement race",
         link_retarget_and_same_target_replacement_fail_closed},
#endif
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
