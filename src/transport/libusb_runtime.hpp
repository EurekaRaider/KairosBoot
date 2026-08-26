#pragma once

#include "src/transport/transfer_ring.hpp"

#include <libusb.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kairosboot::transport {

inline constexpr std::uint16_t kRequiredLibusbMajor = 1;
inline constexpr std::uint16_t kRequiredLibusbMinor = 0;
inline constexpr std::uint16_t kRequiredLibusbMicro = 30;

enum class LibusbSubmitFaultPoint : std::uint8_t {
    pending_allocation,
    fallback_payload_allocation,
    registry_allocation,
};

// Injectable libusb surface. Production uses system(); deterministic tests
// replace every entry without requiring USB hardware.
struct LibusbFunctions final {
    std::function<const libusb_version*()> get_version;
    std::function<int(libusb_context**)> init;
    std::function<void(libusb_context*)> exit;
    std::function<int(libusb_context*, timeval*, int*)> handle_events;
    std::function<void(libusb_context*)> interrupt_events;

    std::function<ssize_t(libusb_context*, libusb_device***)> get_device_list;
    std::function<void(libusb_device**, int)> free_device_list;
    std::function<int(libusb_device*, libusb_device_descriptor*)> get_device_descriptor;
    std::function<int(libusb_device*, libusb_config_descriptor**)> get_active_config_descriptor;
    std::function<int(libusb_device*, std::uint8_t, libusb_config_descriptor**)>
        get_config_descriptor;
    std::function<void(libusb_config_descriptor*)> free_config_descriptor;
    std::function<std::uint8_t(libusb_device*)> get_bus_number;
    std::function<std::uint8_t(libusb_device*)> get_device_address;
    std::function<int(libusb_device*, std::uint8_t*, int)> get_port_numbers;
    std::function<int(libusb_device*, libusb_device_string_type, char*, int)>
        get_device_string;

    std::function<int(libusb_device*, libusb_device_handle**)> open;
    std::function<void(libusb_device_handle*)> close;
    std::function<int(libusb_device_handle*, int)> claim_interface;
    std::function<int(libusb_device_handle*, int)> release_interface;
    std::function<int(libusb_device_handle*, int, int)> set_interface_alt_setting;

    std::function<libusb_transfer*(int)> alloc_transfer;
    std::function<int(libusb_transfer*)> submit_transfer;
    std::function<int(libusb_transfer*)> cancel_transfer;
    std::function<void(libusb_transfer*)> free_transfer;

    // Optional deterministic allocation-fault seam for transport tests. The
    // production function table leaves it empty.
    std::function<void(LibusbSubmitFaultPoint)> submit_allocation_fault;

    [[nodiscard]] static LibusbFunctions system();
    [[nodiscard]] bool complete() const noexcept;
};

enum class LibusbRuntimeErrorKind : std::uint8_t {
    invalid_function_table,
    version_mismatch,
    already_running,
    init_failed,
    event_thread_failed,
    event_loop_failed,
    runtime_stopped,
    enumeration_failed,
    invalid_device,
    device_not_found,
    open_failed,
    claim_failed,
    alternate_setting_failed,
};

struct LibusbRuntimeError final {
    LibusbRuntimeErrorKind kind{LibusbRuntimeErrorKind::init_failed};
    int native_code{};
    std::uint16_t actual_major{};
    std::uint16_t actual_minor{};
    std::uint16_t actual_micro{};
};

struct UsbInterfaceFilter final {
    std::optional<std::uint16_t> vendor_id;
    std::optional<std::uint16_t> product_id;
    std::optional<std::uint8_t> interface_class;
    std::optional<std::uint8_t> interface_subclass;
    std::optional<std::uint8_t> interface_protocol;
};

struct UsbDeviceInfo final {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint8_t bus_number{};
    std::uint8_t device_address{};
    std::vector<std::uint8_t> port_path;
    std::string serial_utf8;
    std::uint8_t interface_number{};
    std::uint8_t alternate_setting{};
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};
    std::uint8_t bulk_out_endpoint{};
    std::uint16_t bulk_out_max_packet_size{};
};

enum class ZeroPacketPolicy : std::uint8_t {
    never,
    when_logical_message_end_packet_aligned,
};

struct BulkOutOptions final {
    std::uint32_t timeout_ms{};
    // Fastboot data rings must retain the default. The opt-in policy is only
    // evaluated for TransferSubmission::logical_message_end and emits one
    // explicit zero-length transfer after that complete logical message.
    // This contract has not yet been validated on real Windows/Linux/macOS USB hardware.
    ZeroPacketPolicy zero_packet{ZeroPacketPolicy::never};
};

class LibusbBulkOutBackend;

class LibusbRuntime final {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<LibusbRuntime>, LibusbRuntimeError>
    create(LibusbFunctions functions = LibusbFunctions::system());

    LibusbRuntime(const LibusbRuntime&) = delete;
    LibusbRuntime& operator=(const LibusbRuntime&) = delete;
    ~LibusbRuntime();

    [[nodiscard]] bool running() const noexcept;
    void stop() noexcept;

    [[nodiscard]] std::expected<std::vector<UsbDeviceInfo>, LibusbRuntimeError> enumerate(
        const UsbInterfaceFilter& filter) const;
    [[nodiscard]] std::expected<std::unique_ptr<LibusbBulkOutBackend>, LibusbRuntimeError>
    open_bulk_out(const UsbDeviceInfo& device, BulkOutOptions options = {});

    [[nodiscard]] std::optional<int> last_event_error() const noexcept;
    [[nodiscard]] std::thread::id event_thread_id() const noexcept;
    [[nodiscard]] bool shutdown_quarantined() const noexcept;

private:
    friend class LibusbBulkOutBackend;
    struct State;
    explicit LibusbRuntime(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

class LibusbBulkOutBackend final : public TransferBackend {
public:
    LibusbBulkOutBackend(const LibusbBulkOutBackend&) = delete;
    LibusbBulkOutBackend& operator=(const LibusbBulkOutBackend&) = delete;
    ~LibusbBulkOutBackend() override;

    [[nodiscard]] SubmitResult submit(const TransferSubmission& submission) noexcept override;
    void cancel(TransferId id) noexcept override;

    [[nodiscard]] bool try_pop_completion(TransferCompletion& completion);
    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] bool shutdown_quarantined() const noexcept;
    void stop() noexcept;

private:
    friend class LibusbRuntime;
    struct State;
    explicit LibusbBulkOutBackend(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

}  // namespace kairosboot::transport
