#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#endif

#include "src/transport/linux_usb_topology.hpp"
#include "src/transport/macos_usb_topology.hpp"
#include "src/transport/transfer_ring.hpp"
#include "src/transport/windows_usb_topology.hpp"

#include <libusb.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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

// A verified open is cancellable only at these owner-controlled boundaries.
// The libusb open/configuration/claim calls between boundaries are synchronous
// native calls and have no portable preemptive cancellation API.
enum class LibusbVerifiedOpenStage : std::uint8_t {
    none,
    reservation,
    selection,
    native_open,
    configuration,
    claim,
    post_open_identity,
    publish,
};

enum class LibusbOpenCancellationGuarantee : std::uint8_t {
    not_applicable,
    cooperative_stage_boundary,
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
    std::function<unsigned long(libusb_device*)> get_session_data;
    std::function<int(libusb_device*, std::uint8_t*, int)> get_port_numbers;
    std::function<int(libusb_device*, libusb_device_string_type, char*, int)>
        get_device_string;

    std::function<int(libusb_device*, libusb_device_handle**)> open;
    std::function<libusb_device*(libusb_device_handle*)> get_device;
    std::function<void(libusb_device_handle*)> close;
    std::function<int(libusb_device_handle*, int*)> get_configuration;
    std::function<int(libusb_device_handle*, int)> set_configuration;
    std::function<int(libusb_device_handle*, int)> claim_interface;
    std::function<int(libusb_device_handle*, int)> release_interface;
    std::function<int(libusb_device_handle*, int, int)> set_interface_alt_setting;

    std::function<libusb_transfer*(int)> alloc_transfer;
    std::function<int(libusb_transfer*)> submit_transfer;
    std::function<int(libusb_transfer*)> cancel_transfer;
    std::function<void(libusb_transfer*)> free_transfer;

    // Quarantine may outlive the caller's reference to the SDK shared library.
    // Production pins the module containing this runtime; tests may inject a
    // deterministic result. The production failure handler never returns.
    std::function<bool()> pin_current_module;
    std::function<void()> module_pin_failure;

    // Optional deterministic allocation-fault seam for transport tests. The
    // production function table leaves it empty.
    std::function<void(LibusbSubmitFaultPoint)> submit_allocation_fault;

    // Optional verified-open stage seam. Tests may block a boundary to prove
    // that cancellation/deadline expiry observed after a synchronous native
    // call fails closed. Production leaves it empty. Throwing is treated as an
    // open failure; callbacks must not re-enter this runtime.
    std::function<void(LibusbVerifiedOpenStage)> verified_open_stage_observer;

    // Optional platform topology enrichment. Production installs each resolver
    // only on its host platform; tests may inject either resolver anywhere.
    std::function<std::expected<LinuxUsbTopology, LinuxUsbTopologyError>(
        const LinuxUsbTopologyQuery&)>
        resolve_linux_topology;
    std::function<std::expected<std::vector<WindowsUsbTopologyResult>,
                                WindowsUsbTopologyError>(
        std::span<const WindowsUsbTopologyQuery>,
        std::chrono::steady_clock::time_point,
        std::stop_token)>
        resolve_windows_topology;
    // Required whenever resolve_windows_topology is installed. This captures
    // the exact PnP instance ID for libusb's DEVINST before the remaining
    // libusb snapshot is read, so later topology validation can reject DEVINST
    // reuse instead of combining two device generations.
    std::function<std::expected<std::string, WindowsUsbTopologyError>(
        unsigned long,
        std::chrono::steady_clock::time_point,
        std::stop_token)>
        capture_windows_session_identity;

    // Optional macOS topology enrichment. One call receives every selected
    // device; each batch entry contains all selected interfaces for that
    // libusb device.
    // The resolver must preserve device/interface order and publish each
    // device from one consistent generation. Tests may inject this on any
    // host; production installs the IOKit-backed resolver only on Apple.
    std::function<std::expected<std::vector<MacUsbTopologyDeviceResult>,
                                MacUsbTopologyError>(
        std::span<const MacUsbTopologyDeviceQuery>,
        MacUsbTopologyTimePoint,
        std::stop_token)>
        resolve_macos_topology;

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
    configuration_failed,
    interface_busy,
    claim_failed,
    alternate_setting_failed,
    operation_cancelled,
    operation_timed_out,
    identity_changed,
};

struct LibusbRuntimeError final {
    LibusbRuntimeErrorKind kind{LibusbRuntimeErrorKind::init_failed};
    int native_code{};
    std::uint16_t actual_major{};
    std::uint16_t actual_minor{};
    std::uint16_t actual_micro{};
    LibusbVerifiedOpenStage verified_open_stage{LibusbVerifiedOpenStage::none};
    LibusbOpenCancellationGuarantee cancellation_guarantee{
        LibusbOpenCancellationGuarantee::not_applicable};
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
    // Opaque backend identity captured from libusb 1.0.30. On Darwin this is
    // the IOKit sessionID used only to correlate an immutable registry
    // snapshot; other platforms must not treat it as a physical-port key.
    std::uint64_t backend_session_id{};
    std::uint8_t configuration_value{};
    std::vector<std::uint8_t> port_path;
    std::string serial_utf8;
    std::uint8_t interface_number{};
    std::uint8_t alternate_setting{};
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};
    std::uint8_t bulk_out_endpoint{};
    std::uint16_t bulk_out_max_packet_size{};
    std::uint8_t bulk_in_endpoint{};
    std::uint16_t bulk_in_max_packet_size{};
    std::optional<LinuxUsbTopology> linux_topology;
    std::optional<LinuxUsbTopologyError> linux_topology_error;
    std::optional<WindowsUsbTopology> windows_topology;
    std::optional<WindowsUsbTopologyError> windows_topology_error;
    std::optional<MacUsbTopology> macos_topology;
    std::optional<MacUsbTopologyError> macos_topology_error;
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

// Move-only proof that the returned backend owns the exact interface described
// by verified_identity(). The identity is reconstructed after open, claim, and
// alternate selection from libusb_get_device(handle), not copied from the
// caller's enumeration snapshot. Platform topology is resolved again from
// that claimed generation, including its current backend session and device
// address. Where the platform exposes a generation identity, the full claimed
// descriptor/interface/endpoint snapshot is bracketed by that identity before
// topology is published. verified_identity() retains either the new topology
// value or its detailed platform diagnostic.
class LibusbVerifiedOpenResult final {
public:
    static constexpr LibusbOpenCancellationGuarantee cancellation_guarantee =
        LibusbOpenCancellationGuarantee::cooperative_stage_boundary;

    LibusbVerifiedOpenResult(const LibusbVerifiedOpenResult&) = delete;
    LibusbVerifiedOpenResult& operator=(const LibusbVerifiedOpenResult&) = delete;
    LibusbVerifiedOpenResult(LibusbVerifiedOpenResult&&) noexcept;
    LibusbVerifiedOpenResult& operator=(LibusbVerifiedOpenResult&&) noexcept;
    ~LibusbVerifiedOpenResult();

    [[nodiscard]] const UsbDeviceInfo& verified_identity() const noexcept;
    [[nodiscard]] LibusbBulkOutBackend& backend() const noexcept;
    [[nodiscard]] std::unique_ptr<LibusbBulkOutBackend> take_backend() noexcept;

private:
    friend class LibusbRuntime;
    LibusbVerifiedOpenResult(std::unique_ptr<LibusbBulkOutBackend> backend,
                             UsbDeviceInfo verified_identity) noexcept;

    std::unique_ptr<LibusbBulkOutBackend> backend_;
    UsbDeviceInfo verified_identity_;
};

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
    // Cancellation/deadline checks happen before publication and at every
    // LibusbVerifiedOpenStage boundary. A synchronous native call already in
    // progress is allowed to return; therefore this is not an end-to-end
    // wall-clock bound on libusb_open/configuration/claim. No detached worker
    // is created. After a boundary observes cancellation or timeout, the call
    // returns only after the handle is synchronously released/closed and the
    // process-wide interface reservation has been released.
    [[nodiscard]] std::expected<LibusbVerifiedOpenResult, LibusbRuntimeError>
    open_bulk_out_verified(
        const UsbDeviceInfo& device,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::stop_token cancellation = {},
        BulkOutOptions options = {});

    [[nodiscard]] std::optional<int> last_event_error() const noexcept;
    [[nodiscard]] std::thread::id event_thread_id() const noexcept;
    [[nodiscard]] bool shutdown_quarantined() const noexcept;
    [[nodiscard]] bool quarantine_module_pin_failed() const noexcept;

private:
    friend class LibusbBulkOutBackend;
    struct State;
    explicit LibusbRuntime(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

// Owns one claimed Fastboot interface. It remains a TransferBackend for the
// high-throughput OUT ring and also supplies one-at-a-time logical IN reads to
// the protocol adapter; both directions share the same drain/quarantine owner.
class LibusbBulkOutBackend final : public TransferBackend {
public:
    enum class WaitCode : std::uint8_t {
        completion,
        timeout,
        stopped,
        event_error,
    };

    struct WaitResult final {
        WaitCode code{WaitCode::stopped};
        TransferCompletion completion;
    };

    enum class ReadCode : std::uint8_t {
        success,
        timeout,
        cancelled,
        no_device,
        stall,
        overflow,
        resource_exhausted,
        io_error,
        closed,
    };

    struct ReadResult final {
        ReadCode code{ReadCode::io_error};
        std::size_t transferred{};
        bool truncated{false};
    };

    LibusbBulkOutBackend(const LibusbBulkOutBackend&) = delete;
    LibusbBulkOutBackend& operator=(const LibusbBulkOutBackend&) = delete;
    ~LibusbBulkOutBackend() override;

    [[nodiscard]] SubmitResult submit(const TransferSubmission& submission) noexcept override;
    void cancel(TransferId id) noexcept override;

    [[nodiscard]] bool try_pop_completion(TransferCompletion& completion);
    [[nodiscard]] WaitResult wait_for_completion(
        std::chrono::milliseconds timeout);
    [[nodiscard]] ReadResult read_logical_response(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout);
    [[nodiscard]] ReadResult read_data(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] bool shutdown_quarantined() const noexcept;
    // Signals native transfers without waiting for their completion callbacks.
    // stop() remains the owner of bounded drain and quarantine.
    void request_stop() noexcept;
    void stop() noexcept;

private:
    friend class LibusbRuntime;
    struct State;
    explicit LibusbBulkOutBackend(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

}  // namespace kairosboot::transport
