#include "src/transport/libusb_runtime.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace kairosboot::transport {

namespace {

// These bounds apply only to KairosBoot's waits after native libusb calls
// return. libusb_cancel_transfer(), libusb_interrupt_event_handler(), and other
// native calls are not preempted by these timers; this is not an end-to-end SLA.
inline constexpr auto kPerBackendDrainWait = std::chrono::milliseconds{250};
inline constexpr auto kRuntimeDrainWait = std::chrono::milliseconds{250};
inline constexpr auto kEventThreadExitWait = std::chrono::milliseconds{250};

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] SteadyClock::time_point deadline_after(
    const std::chrono::milliseconds timeout) noexcept {
    const auto now = SteadyClock::now();
    if (timeout == std::chrono::milliseconds::max()) {
        return SteadyClock::time_point::max();
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        return now;
    }
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::time_point::max() - now);
    return timeout >= room ? SteadyClock::time_point::max() : now + timeout;
}

[[nodiscard]] std::uint32_t libusb_timeout_until(
    const SteadyClock::time_point deadline) noexcept {
    if (deadline == SteadyClock::time_point::max()) {
        return 0;
    }
    const auto now = SteadyClock::now();
    if (now >= deadline) {
        return 1;
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(remaining),
        std::numeric_limits<std::uint32_t>::max()));
}

constinit std::byte kModulePinAnchor{};

#if defined(_WIN32)
[[nodiscard]] bool pin_runtime_module() noexcept {
    HMODULE module = nullptr;
    const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (GetModuleHandleExW(flags,
                           reinterpret_cast<LPCWSTR>(&kModulePinAnchor),
                           &module) != FALSE) {
        return true;
    }

    // The main executable is intrinsically mapped for process lifetime.
    MEMORY_BASIC_INFORMATION information{};
    return VirtualQuery(&kModulePinAnchor, &information, sizeof(information)) != 0 &&
           information.AllocationBase ==
               reinterpret_cast<void*>(GetModuleHandleW(nullptr));
}
#else
[[nodiscard]] bool same_file(const char* const left, const char* const right) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    struct stat left_status {};
    struct stat right_status {};
    return stat(left, &left_status) == 0 && stat(right, &right_status) == 0 &&
           left_status.st_dev == right_status.st_dev &&
           left_status.st_ino == right_status.st_ino;
}

[[nodiscard]] bool module_is_main_executable(const char* const module_path) noexcept {
    std::array<char, PATH_MAX + 1> executable_path{};
#if defined(__APPLE__)
    auto size = static_cast<std::uint32_t>(executable_path.size());
    if (_NSGetExecutablePath(executable_path.data(), &size) != 0) {
        return false;
    }
#elif defined(__linux__)
    const auto length = readlink("/proc/self/exe",
                                 executable_path.data(),
                                 executable_path.size() - 1U);
    if (length <= 0 || static_cast<std::size_t>(length) >= executable_path.size()) {
        return false;
    }
    executable_path[static_cast<std::size_t>(length)] = '\0';
#else
    return false;
#endif
    return same_file(module_path, executable_path.data());
}

[[nodiscard]] bool pin_runtime_module() noexcept {
    Dl_info information{};
    if (dladdr(&kModulePinAnchor, &information) == 0 ||
        information.dli_fname == nullptr || information.dli_fname[0] == '\0') {
        return false;
    }
    if (module_is_main_executable(information.dli_fname)) {
        return true;
    }

    auto flags = RTLD_NOW | RTLD_LOCAL;
#if defined(RTLD_NODELETE)
    flags |= RTLD_NODELETE;
#endif
    // Deliberately do not dlclose: the extra reference keeps KairosBoot and its
    // linked libusb dependency resident until process termination.
    return dlopen(information.dli_fname, flags) != nullptr;
}
#endif

[[nodiscard]] bool version_is_exact(const libusb_version* version) noexcept {
    return version != nullptr && version->major == kRequiredLibusbMajor &&
           version->minor == kRequiredLibusbMinor && version->micro == kRequiredLibusbMicro &&
           (version->rc == nullptr || version->rc[0] == '\0');
}

[[nodiscard]] SubmitResult classify_submit_error(const int code) noexcept {
    if (code == LIBUSB_ERROR_NO_DEVICE) {
        return SubmitResult::no_device;
    }
    if (code == LIBUSB_ERROR_NO_MEM) {
        return SubmitResult::resource_exhausted;
    }
    return SubmitResult::io_error;
}

[[nodiscard]] CompletionCode classify_completion(
    const libusb_transfer_status status) noexcept {
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return CompletionCode::success;
        case LIBUSB_TRANSFER_TIMED_OUT:
            return CompletionCode::timeout;
        case LIBUSB_TRANSFER_CANCELLED:
            return CompletionCode::cancelled;
        case LIBUSB_TRANSFER_STALL:
            return CompletionCode::stall;
        case LIBUSB_TRANSFER_NO_DEVICE:
            return CompletionCode::no_device;
        case LIBUSB_TRANSFER_ERROR:
        case LIBUSB_TRANSFER_OVERFLOW:
            return CompletionCode::io_error;
    }
    return CompletionCode::io_error;
}

[[nodiscard]] bool matches(const std::optional<std::uint16_t>& expected,
                           const std::uint16_t actual) noexcept {
    return !expected.has_value() || *expected == actual;
}

[[nodiscard]] bool matches(const std::optional<std::uint8_t>& expected,
                           const std::uint8_t actual) noexcept {
    return !expected.has_value() || *expected == actual;
}

struct DeviceListGuard final {
    const LibusbFunctions* functions{};
    libusb_device** list{};

    ~DeviceListGuard() {
        if (list != nullptr) {
            functions->free_device_list(list, 1);
        }
    }
};

struct ConfigDescriptorGuard final {
    const LibusbFunctions* functions{};
    libusb_config_descriptor* descriptor{};

    ~ConfigDescriptorGuard() {
        if (descriptor != nullptr) {
            functions->free_config_descriptor(descriptor);
        }
    }
};

struct UsbInterfaceReservationKey final {
    std::uint8_t bus_number{};
    std::vector<std::uint8_t> port_path;
    std::uint8_t interface_number{};
};

[[nodiscard]] bool operator<(const UsbInterfaceReservationKey& left,
                             const UsbInterfaceReservationKey& right) noexcept {
    if (left.bus_number != right.bus_number) {
        return left.bus_number < right.bus_number;
    }
    if (left.port_path != right.port_path) {
        return std::lexicographical_compare(left.port_path.begin(),
                                            left.port_path.end(),
                                            right.port_path.begin(),
                                            right.port_path.end());
    }
    return left.interface_number < right.interface_number;
}

[[nodiscard]] UsbInterfaceReservationKey reservation_key(
    const UsbDeviceInfo& device) {
    return UsbInterfaceReservationKey{
        device.bus_number,
        device.port_path,
        device.interface_number,
    };
}

struct ProcessLifetimeQuarantineRoot final {
    std::mutex mutex;
    std::shared_ptr<void>* runtime_slot{};
};

[[nodiscard]] ProcessLifetimeQuarantineRoot& process_lifetime_quarantine_root() {
    static ProcessLifetimeQuarantineRoot root;
    return root;
}

[[nodiscard]] bool interface_snapshot_matches(const LibusbFunctions& functions,
                                              libusb_device* const candidate,
                                              const UsbDeviceInfo& snapshot) {
    libusb_config_descriptor* raw_config = nullptr;
    auto result = functions.get_active_config_descriptor(candidate, &raw_config);
    if (result != LIBUSB_SUCCESS) {
        result = functions.get_config_descriptor(candidate, 0, &raw_config);
    }
    if (result != LIBUSB_SUCCESS || raw_config == nullptr) {
        return false;
    }
    ConfigDescriptorGuard config_guard{&functions, raw_config};
    if (raw_config->bConfigurationValue != snapshot.configuration_value) {
        return false;
    }
    if (raw_config->bNumInterfaces != 0 && raw_config->interface == nullptr) {
        return false;
    }

    for (std::uint8_t interface_index = 0;
         interface_index < raw_config->bNumInterfaces;
         ++interface_index) {
        const auto& interface = raw_config->interface[interface_index];
        if (interface.num_altsetting <= 0 || interface.altsetting == nullptr) {
            continue;
        }
        for (int alternate_index = 0;
             alternate_index < interface.num_altsetting;
             ++alternate_index) {
            const auto& alternate = interface.altsetting[alternate_index];
            if (alternate.bInterfaceNumber != snapshot.interface_number ||
                alternate.bAlternateSetting != snapshot.alternate_setting ||
                alternate.bInterfaceClass != snapshot.interface_class ||
                alternate.bInterfaceSubClass != snapshot.interface_subclass ||
                alternate.bInterfaceProtocol != snapshot.interface_protocol ||
                (alternate.bNumEndpoints != 0 && alternate.endpoint == nullptr)) {
                continue;
            }
            bool matched_out = false;
            bool matched_in = false;
            for (std::uint8_t endpoint_index = 0;
                 endpoint_index < alternate.bNumEndpoints;
                 ++endpoint_index) {
                const auto& endpoint = alternate.endpoint[endpoint_index];
                const auto type = endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                const auto maximum_packet_size =
                    static_cast<std::uint16_t>(endpoint.wMaxPacketSize & 0x07FFU);
                if (type != LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK) {
                    continue;
                }
                const auto direction =
                    endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN;
                matched_out = matched_out ||
                    (direction == LIBUSB_ENDPOINT_OUT &&
                     endpoint.bEndpointAddress == snapshot.bulk_out_endpoint &&
                     maximum_packet_size == snapshot.bulk_out_max_packet_size);
                matched_in = matched_in ||
                    (direction == LIBUSB_ENDPOINT_IN &&
                     endpoint.bEndpointAddress == snapshot.bulk_in_endpoint &&
                     maximum_packet_size == snapshot.bulk_in_max_packet_size);
            }
            if (matched_out && matched_in) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool open_snapshot_matches(const LibusbFunctions& functions,
                                         libusb_device* const candidate,
                                         const UsbDeviceInfo& snapshot) {
    libusb_device_descriptor descriptor{};
    if (functions.get_device_descriptor(candidate, &descriptor) != LIBUSB_SUCCESS ||
        descriptor.idVendor != snapshot.vendor_id ||
        descriptor.idProduct != snapshot.product_id ||
        functions.get_bus_number(candidate) != snapshot.bus_number) {
        return false;
    }

    std::array<std::uint8_t, 16> candidate_ports{};
    const auto port_count = functions.get_port_numbers(
        candidate, candidate_ports.data(), static_cast<int>(candidate_ports.size()));
    if (port_count <= 0 ||
        static_cast<std::size_t>(port_count) != snapshot.port_path.size() ||
        !std::equal(snapshot.port_path.begin(),
                    snapshot.port_path.end(),
                    candidate_ports.begin())) {
        return false;
    }

    // An empty serial snapshot means the device did not expose a readable
    // serial during enumeration. A non-empty snapshot is a mandatory secondary
    // identity check and cannot be replaced by the transient USB address.
    if (!snapshot.serial_utf8.empty()) {
        std::array<char, LIBUSB_DEVICE_STRING_BYTES_MAX> serial_buffer{};
        const auto serial_result = functions.get_device_string(
            candidate,
            LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
            serial_buffer.data(),
            static_cast<int>(serial_buffer.size()));
        if (serial_result <= 0) {
            return false;
        }
        const auto serial_end =
            std::find(serial_buffer.begin(), serial_buffer.end(), '\0');
        if (std::string_view(serial_buffer.data(),
                             static_cast<std::size_t>(serial_end - serial_buffer.begin())) !=
            snapshot.serial_utf8) {
            return false;
        }
    }

    return interface_snapshot_matches(functions, candidate, snapshot);
}

}  // namespace

LibusbFunctions LibusbFunctions::system() {
    LibusbFunctions functions;
    functions.get_version = [] { return libusb_get_version(); };
    functions.init = [](libusb_context** context) { return libusb_init(context); };
    functions.exit = [](libusb_context* context) { libusb_exit(context); };
    functions.handle_events = [](libusb_context* context, timeval* timeout, int* completed) {
        return libusb_handle_events_timeout_completed(context, timeout, completed);
    };
    functions.interrupt_events = [](libusb_context* context) {
        libusb_interrupt_event_handler(context);
    };
    functions.get_device_list = [](libusb_context* context, libusb_device*** list) {
        return libusb_get_device_list(context, list);
    };
    functions.free_device_list = [](libusb_device** list, const int unref) {
        libusb_free_device_list(list, unref);
    };
    functions.get_device_descriptor = [](libusb_device* device,
                                         libusb_device_descriptor* descriptor) {
        return libusb_get_device_descriptor(device, descriptor);
    };
    functions.get_active_config_descriptor = [](libusb_device* device,
                                                libusb_config_descriptor** descriptor) {
        return libusb_get_active_config_descriptor(device, descriptor);
    };
    functions.get_config_descriptor = [](libusb_device* device,
                                         const std::uint8_t index,
                                         libusb_config_descriptor** descriptor) {
        return libusb_get_config_descriptor(device, index, descriptor);
    };
    functions.free_config_descriptor = [](libusb_config_descriptor* descriptor) {
        libusb_free_config_descriptor(descriptor);
    };
    functions.get_bus_number = [](libusb_device* device) {
        return libusb_get_bus_number(device);
    };
    functions.get_device_address = [](libusb_device* device) {
        return libusb_get_device_address(device);
    };
    functions.get_port_numbers = [](libusb_device* device,
                                    std::uint8_t* path,
                                    const int length) {
        return libusb_get_port_numbers(device, path, length);
    };
    functions.get_device_string = [](libusb_device* device,
                                     const libusb_device_string_type type,
                                     char* output,
                                     const int length) {
        return libusb_get_device_string(device, type, output, length);
    };
    functions.open = [](libusb_device* device, libusb_device_handle** handle) {
        return libusb_open(device, handle);
    };
    functions.close = [](libusb_device_handle* handle) { libusb_close(handle); };
    functions.get_configuration = [](libusb_device_handle* handle, int* configuration) {
        return libusb_get_configuration(handle, configuration);
    };
    functions.set_configuration = [](libusb_device_handle* handle, const int configuration) {
        return libusb_set_configuration(handle, configuration);
    };
    functions.claim_interface = [](libusb_device_handle* handle, const int interface_number) {
        return libusb_claim_interface(handle, interface_number);
    };
    functions.release_interface = [](libusb_device_handle* handle,
                                     const int interface_number) {
        return libusb_release_interface(handle, interface_number);
    };
    functions.set_interface_alt_setting = [](libusb_device_handle* handle,
                                             const int interface_number,
                                             const int alternate_setting) {
        return libusb_set_interface_alt_setting(handle, interface_number, alternate_setting);
    };
    functions.alloc_transfer = [](const int packets) { return libusb_alloc_transfer(packets); };
    functions.submit_transfer = [](libusb_transfer* transfer) {
        return libusb_submit_transfer(transfer);
    };
    functions.cancel_transfer = [](libusb_transfer* transfer) {
        return libusb_cancel_transfer(transfer);
    };
    functions.free_transfer = [](libusb_transfer* transfer) {
        libusb_free_transfer(transfer);
    };
    functions.pin_current_module = [] { return pin_runtime_module(); };
    functions.module_pin_failure = [] { std::terminate(); };
    return functions;
}

bool LibusbFunctions::complete() const noexcept {
    return get_version && init && exit && handle_events && interrupt_events &&
           get_device_list && free_device_list && get_device_descriptor &&
           get_active_config_descriptor && get_config_descriptor && free_config_descriptor &&
           get_bus_number && get_device_address && get_port_numbers && get_device_string &&
           open && close && get_configuration && set_configuration && claim_interface &&
           release_interface &&
           set_interface_alt_setting && alloc_transfer && submit_transfer &&
           cancel_transfer && free_transfer && pin_current_module &&
           module_pin_failure;
}

struct LibusbRuntime::State final : std::enable_shared_from_this<LibusbRuntime::State> {
    State(LibusbFunctions table,
          libusb_context* initialized_context,
          ProcessLifetimeQuarantineRoot* quarantine_root_value,
          std::unique_ptr<std::shared_ptr<void>> quarantine_slot_value)
        : functions(std::move(table)),
          context(initialized_context),
          quarantine_root(quarantine_root_value),
          quarantine_slot(std::move(quarantine_slot_value)) {}

    LibusbFunctions functions;
    libusb_context* context{};
    ProcessLifetimeQuarantineRoot* quarantine_root{};
    std::unique_ptr<std::shared_ptr<void>> quarantine_slot;
    std::atomic<bool> running{false};
    std::atomic<bool> accepting{false};
    std::atomic<bool> stop_events{false};
    std::atomic<bool> event_terminal{false};
    std::atomic<bool> event_exited{false};
    std::atomic<bool> quarantined{false};
    std::atomic<bool> module_pin_attempted{false};
    std::atomic<bool> module_pin_failed{false};
    std::atomic<int> event_error{0};
    std::atomic<std::uint64_t> next_event_epoch{1};
    std::atomic<std::uint64_t> active_event_epoch{0};
    std::atomic<std::uint64_t> completed_event_epoch{0};
    std::thread event_thread;
    mutable std::mutex event_identity_mutex;
    std::thread::id event_identity;
    std::mutex event_exit_mutex;
    std::condition_variable event_exit_cv;
    mutable std::mutex stop_mutex;
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    std::mutex backends_mutex;
    std::vector<std::weak_ptr<LibusbBulkOutBackend::State>> backends;
    std::shared_ptr<LibusbBulkOutBackend::State> quarantined_backend_head;
    std::mutex reservations_mutex;
    std::set<UsbInterfaceReservationKey> reserved_interfaces;

    void event_loop() noexcept;
    void register_backend(const std::shared_ptr<LibusbBulkOutBackend::State>& backend);
    void stop_backend(const std::shared_ptr<LibusbBulkOutBackend::State>& backend) noexcept;
    void stop_all() noexcept;
    void quarantine_backend(const std::shared_ptr<LibusbBulkOutBackend::State>& backend,
                            bool global_runtime_quarantine) noexcept;
    [[nodiscard]] std::expected<UsbInterfaceReservationKey, LibusbRuntimeError>
    reserve_interface(const UsbDeviceInfo& device);
    void release_interface_reservation(
        const UsbInterfaceReservationKey& key) noexcept;
    void root_process_lifetime_quarantine(bool global_runtime_quarantine) noexcept;
    [[nodiscard]] bool wait_for_event_exit(
        std::chrono::steady_clock::time_point deadline) noexcept;
};

struct LibusbBulkOutBackend::State final {
    enum class Phase : std::uint8_t {
        data,
        zero_packet,
    };

    struct RawCompletion final {
        TransferId id{};
        libusb_transfer_status status{LIBUSB_TRANSFER_ERROR};
        int actual_length{};
        std::uint64_t event_epoch{};
    };

    struct Pending final {
        State* owner{};
        TransferId id{};
        libusb_transfer* transfer{};
        std::size_t requested_bytes{};
        std::size_t data_transferred{};
        bool needs_zero_packet{false};
        Phase phase{Phase::data};
        std::shared_ptr<const void> payload_lifetime;
        std::vector<std::byte> fallback_payload;
        RawCompletion raw_completion;
        Pending* next_completed{};
        TransferCompletion ready_completion;
        Pending* next_ready{};
    };

    struct ReadPending final {
        struct RawCompletion final {
            libusb_transfer_status status{LIBUSB_TRANSFER_ERROR};
            int actual_length{};
            std::uint64_t event_epoch{};
        };

        State* owner{};
        libusb_transfer* transfer{};
        std::vector<std::byte> buffer;
        std::size_t destination_capacity{};
        SteadyClock::time_point deadline{};
        std::atomic<bool> raw_ready{false};
        RawCompletion raw_completion;
    };

    struct ReadyRead final {
        LibusbBulkOutBackend::ReadResult result;
        std::vector<std::byte> bytes;
    };

    State(std::shared_ptr<LibusbRuntime::State> owning_runtime,
          libusb_device_handle* opened_handle,
          const UsbDeviceInfo& device,
          const BulkOutOptions backend_options,
          UsbInterfaceReservationKey interface_reservation)
        : runtime(std::move(owning_runtime)),
          handle(opened_handle),
          interface_number(device.interface_number),
          endpoint(device.bulk_out_endpoint),
          inbound_endpoint(device.bulk_in_endpoint),
          maximum_packet_size(device.bulk_out_max_packet_size),
          options(backend_options),
          reservation(std::move(interface_reservation)) {}

    ~State() {
        while (ready_head != nullptr) {
            auto* completed = ready_head;
            ready_head = ready_head->next_ready;
            delete completed;
        }
        if (!closed && pending.empty() && read_pending == nullptr) {
            static_cast<void>(runtime->functions.release_interface(handle, interface_number));
            runtime->functions.close(handle);
            release_reservation_locked();
        }
    }

    std::shared_ptr<LibusbRuntime::State> runtime;
    libusb_device_handle* handle{};
    int interface_number{};
    std::uint8_t endpoint{};
    std::uint8_t inbound_endpoint{};
    std::uint16_t maximum_packet_size{};
    BulkOutOptions options;
    mutable std::mutex mutex;
    bool accepting{true};
    bool closed{false};
    std::atomic<bool> quarantined{false};
    std::unordered_map<TransferId, std::unique_ptr<Pending>> pending;
    std::atomic<Pending*> completion_head{nullptr};
    Pending* completion_fifo_head{};
    Pending* ready_head{};
    Pending* ready_tail{};
    std::unique_ptr<ReadPending> read_pending;
    std::optional<ReadyRead> ready_read;
    std::shared_ptr<State> quarantine_next;
    std::optional<UsbInterfaceReservationKey> reservation;

    static void LIBUSB_CALL on_transfer(libusb_transfer* transfer) noexcept {
        auto* pending_transfer = static_cast<Pending*>(transfer->user_data);
        pending_transfer->owner->enqueue(*pending_transfer, *transfer);
    }

    static void LIBUSB_CALL on_read_transfer(libusb_transfer* transfer) noexcept {
        auto* pending_read = static_cast<ReadPending*>(transfer->user_data);
        pending_read->raw_completion = ReadPending::RawCompletion{
            transfer->status,
            transfer->actual_length,
            pending_read->owner->runtime->active_event_epoch.load(
                std::memory_order_acquire),
        };
        pending_read->raw_ready.store(true, std::memory_order_release);
        pending_read->owner->runtime->completion_cv.notify_all();
    }

    void enqueue(Pending& pending_transfer, const libusb_transfer& transfer) noexcept {
        pending_transfer.raw_completion = RawCompletion{
            pending_transfer.id,
            transfer.status,
            transfer.actual_length,
            runtime->active_event_epoch.load(std::memory_order_acquire),
        };
        auto* head = completion_head.load(std::memory_order_relaxed);
        do {
            pending_transfer.next_completed = head;
        } while (!completion_head.compare_exchange_weak(head,
                                                        &pending_transfer,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed));
        runtime->completion_cv.notify_all();
    }

    [[nodiscard]] SubmitResult submit(const TransferSubmission& submission) noexcept;
    void cancel(TransferId id) noexcept;
    void request_stop() noexcept;
    void service_raw(bool shutting_down);
    [[nodiscard]] bool service_shutdown();
    [[nodiscard]] bool try_pop(TransferCompletion& completion);
    [[nodiscard]] LibusbBulkOutBackend::WaitResult wait_pop(
        std::chrono::milliseconds timeout);
    [[nodiscard]] LibusbBulkOutBackend::ReadResult read_logical_response(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t in_flight() const noexcept;
    void close_if_drained() noexcept;
    void release_reservation_locked() noexcept;

private:
    [[nodiscard]] bool process_one_raw(bool shutting_down);
    [[nodiscard]] bool process_read_raw(bool shutting_down);
    [[nodiscard]] std::optional<ReadyRead> pop_ready_read();
    void configure_read_transfer(ReadPending& pending_read) noexcept;
    void finish_read_locked(LibusbBulkOutBackend::ReadCode code,
                            std::size_t transferred,
                            bool truncated);
    void configure_data_transfer(Pending& pending_transfer,
                                 const TransferSubmission& submission) noexcept;
    void configure_zero_packet(Pending& pending_transfer) noexcept;
    void finish_pending(std::unordered_map<TransferId, std::unique_ptr<Pending>>::iterator entry,
                        CompletionCode code,
                        std::size_t transferred_bytes);
};

void LibusbRuntime::State::event_loop() noexcept {
    {
        std::lock_guard lock(event_identity_mutex);
        event_identity = std::this_thread::get_id();
    }

    while (!stop_events.load(std::memory_order_acquire)) {
        const auto epoch = next_event_epoch.fetch_add(1, std::memory_order_acq_rel);
        active_event_epoch.store(epoch, std::memory_order_release);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100'000;
        int completed = 0;
        int result = LIBUSB_ERROR_OTHER;
        try {
            result = functions.handle_events(context, &timeout, &completed);
        } catch (...) {
            result = LIBUSB_ERROR_OTHER;
        }
        completed_event_epoch.store(epoch, std::memory_order_release);
        completion_cv.notify_all();

        if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) {
            int no_error = 0;
            static_cast<void>(event_error.compare_exchange_strong(no_error,
                                                                   result,
                                                                   std::memory_order_acq_rel,
                                                                   std::memory_order_acquire));
            accepting.store(false, std::memory_order_release);
            event_terminal.store(true, std::memory_order_release);
            break;
        }
    }

    event_exited.store(true, std::memory_order_release);
    event_exit_cv.notify_all();
    completion_cv.notify_all();
}

void LibusbRuntime::State::register_backend(
    const std::shared_ptr<LibusbBulkOutBackend::State>& backend) {
    std::lock_guard lock(backends_mutex);
    backends.erase(std::remove_if(backends.begin(),
                                  backends.end(),
                                  [](const auto& entry) { return entry.expired(); }),
                   backends.end());
    backends.emplace_back(backend);
}

std::expected<UsbInterfaceReservationKey, LibusbRuntimeError>
LibusbRuntime::State::reserve_interface(const UsbDeviceInfo& device) {
    try {
        auto key = reservation_key(device);
        std::lock_guard lock(reservations_mutex);
        const auto [ignored, inserted] = reserved_interfaces.insert(key);
        static_cast<void>(ignored);
        if (!inserted) {
            return std::unexpected(LibusbRuntimeError{
                LibusbRuntimeErrorKind::interface_busy,
                LIBUSB_ERROR_BUSY,
            });
        }
        return std::expected<UsbInterfaceReservationKey, LibusbRuntimeError>{
            std::in_place,
            std::move(key),
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(LibusbRuntimeError{
            LibusbRuntimeErrorKind::open_failed,
            LIBUSB_ERROR_NO_MEM,
        });
    }
}

void LibusbRuntime::State::release_interface_reservation(
    const UsbInterfaceReservationKey& key) noexcept {
    std::lock_guard lock(reservations_mutex);
    reserved_interfaces.erase(key);
}

void LibusbRuntime::State::stop_backend(
    const std::shared_ptr<LibusbBulkOutBackend::State>& backend) noexcept {
    std::unique_lock lifecycle(stop_mutex);
    if (backend->quarantined.load(std::memory_order_acquire)) {
        return;
    }
    backend->request_stop();
    const auto deadline = std::chrono::steady_clock::now() + kPerBackendDrainWait;
    while (!backend->service_shutdown() &&
           !event_terminal.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::unique_lock completion(completion_mutex);
        completion_cv.wait_until(completion, deadline);
    }
    if (!backend->service_shutdown()) {
        quarantine_backend(backend,
                           event_terminal.load(std::memory_order_acquire));
    }
}

void LibusbRuntime::State::stop_all() noexcept {
    std::unique_lock lifecycle(stop_mutex);
    if (!running.load(std::memory_order_acquire)) {
        return;
    }

    accepting.store(false, std::memory_order_release);
    {
        std::lock_guard lock(backends_mutex);
        for (const auto& entry : backends) {
            if (const auto backend = entry.lock()) {
                backend->request_stop();
            }
        }
    }

    const auto drain_deadline =
        std::chrono::steady_clock::now() + kRuntimeDrainWait;
    bool drained = false;
    for (;;) {
        drained = true;
        {
            std::lock_guard lock(backends_mutex);
            for (const auto& entry : backends) {
                if (const auto backend = entry.lock()) {
                    drained = backend->service_shutdown() && drained;
                }
            }
        }
        if (drained) {
            break;
        }
        if (event_terminal.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= drain_deadline) {
            break;
        }
        std::unique_lock completion(completion_mutex);
        completion_cv.wait_until(completion, drain_deadline);
    }

    if (!drained) {
        bool requires_quarantine = false;
        std::lock_guard lock(backends_mutex);
        for (const auto& entry : backends) {
            if (const auto backend = entry.lock();
                backend != nullptr && !backend->service_shutdown()) {
                requires_quarantine = true;
                const auto was_quarantined =
                    backend->quarantined.exchange(true, std::memory_order_acq_rel);
                if (!was_quarantined) {
                    {
                        std::lock_guard backend_lock(backend->mutex);
                        backend->accepting = false;
                        backend->release_reservation_locked();
                    }
                    backend->quarantine_next = std::move(quarantined_backend_head);
                    quarantined_backend_head = backend;
                }
            }
        }
        if (requires_quarantine) {
            root_process_lifetime_quarantine(true);
        }
    }

    stop_events.store(true, std::memory_order_release);
    functions.interrupt_events(context);
    const auto event_exit_deadline =
        std::chrono::steady_clock::now() + kEventThreadExitWait;
    const auto event_stopped = wait_for_event_exit(event_exit_deadline);
    bool event_thread_reaped = !event_thread.joinable();
    if (event_thread.joinable() && event_stopped) {
        try {
            event_thread.join();
            event_thread_reaped = true;
        } catch (const std::system_error&) {
            root_process_lifetime_quarantine(true);
        }
    } else if (event_thread.joinable()) {
        root_process_lifetime_quarantine(true);
        try {
            event_thread.detach();
            event_thread_reaped = true;
        } catch (const std::system_error&) {
            // The process-lifetime root keeps the still-joinable thread object,
            // context, and callback owners alive. Never destroy them unsafely.
        }
    }

    if (event_thread_reaped && !quarantined.load(std::memory_order_acquire)) {
        functions.exit(context);
        context = nullptr;
    } else {
        root_process_lifetime_quarantine(true);
    }
    running.store(false, std::memory_order_release);
    completion_cv.notify_all();
}

void LibusbRuntime::State::quarantine_backend(
    const std::shared_ptr<LibusbBulkOutBackend::State>& backend,
    const bool global_runtime_quarantine) noexcept {
    const auto was_quarantined =
        backend->quarantined.exchange(true, std::memory_order_acq_rel);
    if (!was_quarantined) {
        {
            std::lock_guard backend_lock(backend->mutex);
            backend->accepting = false;
            backend->release_reservation_locked();
        }
        std::lock_guard registry_lock(backends_mutex);
        backend->quarantine_next = std::move(quarantined_backend_head);
        quarantined_backend_head = backend;
    }
    root_process_lifetime_quarantine(global_runtime_quarantine);
}

void LibusbRuntime::State::root_process_lifetime_quarantine(
    const bool global_runtime_quarantine) noexcept {
    if (global_runtime_quarantine) {
        quarantined.store(true, std::memory_order_release);
    }
    if (!module_pin_attempted.exchange(true, std::memory_order_acq_rel)) {
        bool pinned = false;
        try {
            pinned = functions.pin_current_module();
        } catch (...) {
            pinned = false;
        }
        if (!pinned) {
            module_pin_failed.store(true, std::memory_order_release);
            // Production terminates here: returning with callback-capable code
            // that can be unloaded is not a safe degraded mode. Deterministic
            // tests inject a returning handler while running in the main image.
            functions.module_pin_failure();
        }
    }
    if (quarantine_slot == nullptr) {
        return;
    }
    auto self = weak_from_this().lock();
    if (self == nullptr) {
        return;
    }
    std::lock_guard lock(quarantine_root->mutex);
    *quarantine_slot = std::move(self);
    // The raw slot is intentionally process-live. Normal runtimes retain slot
    // ownership and free it; only an unsafe-to-destroy runtime relinquishes it.
    quarantine_root->runtime_slot = quarantine_slot.release();
}

bool LibusbRuntime::State::wait_for_event_exit(
    const std::chrono::steady_clock::time_point deadline) noexcept {
    if (event_exited.load(std::memory_order_acquire)) {
        return true;
    }
    std::unique_lock lock(event_exit_mutex);
    return event_exit_cv.wait_until(lock, deadline, [this] {
        return event_exited.load(std::memory_order_acquire);
    });
}

SubmitResult LibusbBulkOutBackend::State::submit(
    const TransferSubmission& submission) noexcept {
    if (submission.payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return SubmitResult::io_error;
    }

    std::lock_guard lock(mutex);
    if (!accepting || closed || !runtime->accepting.load(std::memory_order_acquire) ||
        pending.contains(submission.id)) {
        return SubmitResult::io_error;
    }

    libusb_transfer* transfer = nullptr;
    std::unique_ptr<Pending> pending_transfer;
    try {
        transfer = runtime->functions.alloc_transfer(0);
        if (transfer == nullptr) {
            return SubmitResult::resource_exhausted;
        }
        if (runtime->functions.submit_allocation_fault) {
            runtime->functions.submit_allocation_fault(
                LibusbSubmitFaultPoint::pending_allocation);
        }
        pending_transfer = std::make_unique<Pending>();
        pending_transfer->owner = this;
        pending_transfer->id = submission.id;
        pending_transfer->transfer = transfer;
        pending_transfer->requested_bytes = submission.payload.size();
        pending_transfer->payload_lifetime = submission.payload_lifetime;
        if (pending_transfer->payload_lifetime == nullptr && !submission.payload.empty()) {
            if (runtime->functions.submit_allocation_fault) {
                runtime->functions.submit_allocation_fault(
                    LibusbSubmitFaultPoint::fallback_payload_allocation);
            }
            pending_transfer->fallback_payload.assign(submission.payload.begin(),
                                                      submission.payload.end());
        }

        configure_data_transfer(*pending_transfer, submission);
        if (runtime->functions.submit_allocation_fault) {
            runtime->functions.submit_allocation_fault(
                LibusbSubmitFaultPoint::registry_allocation);
        }
        // Allocate the registry node before moving ownership. If allocation
        // fails, pending_transfer still owns its metadata and transfer cleanup
        // remains deterministic.
        auto [entry, inserted] = pending.try_emplace(submission.id);
        if (!inserted) {
            runtime->functions.free_transfer(transfer);
            return SubmitResult::io_error;
        }
        entry->second = std::move(pending_transfer);
    } catch (const std::bad_alloc&) {
        if (transfer != nullptr) {
            runtime->functions.free_transfer(transfer);
        }
        return SubmitResult::resource_exhausted;
    } catch (...) {
        if (transfer != nullptr) {
            runtime->functions.free_transfer(transfer);
        }
        return SubmitResult::io_error;
    }

    const auto entry = pending.find(submission.id);
    const auto result = runtime->functions.submit_transfer(transfer);
    if (result != LIBUSB_SUCCESS) {
        auto rejected = std::move(entry->second);
        pending.erase(entry);
        runtime->functions.free_transfer(rejected->transfer);
        return classify_submit_error(result);
    }
    return SubmitResult::accepted;
}

void LibusbBulkOutBackend::State::configure_data_transfer(
    Pending& pending_transfer, const TransferSubmission& submission) noexcept {
    auto payload = submission.payload;
    if (pending_transfer.payload_lifetime == nullptr && !payload.empty()) {
        payload = pending_transfer.fallback_payload;
    }

    auto* transfer = pending_transfer.transfer;
    transfer->dev_handle = handle;
    transfer->flags = 0;
    transfer->endpoint = endpoint;
    transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
    transfer->timeout = options.timeout_ms;
    transfer->length = static_cast<int>(payload.size());
    transfer->actual_length = 0;
    transfer->callback = &State::on_transfer;
    transfer->user_data = &pending_transfer;
    transfer->buffer = reinterpret_cast<unsigned char*>(
        const_cast<std::byte*>(payload.data()));
    pending_transfer.needs_zero_packet =
        options.zero_packet ==
            ZeroPacketPolicy::when_logical_message_end_packet_aligned &&
        submission.logical_message_end &&
        maximum_packet_size != 0 && !payload.empty() &&
        payload.size() % maximum_packet_size == 0;
}

void LibusbBulkOutBackend::State::configure_zero_packet(Pending& pending_transfer) noexcept {
    auto* transfer = pending_transfer.transfer;
    transfer->flags = 0;
    transfer->endpoint = endpoint;
    transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
    transfer->timeout = options.timeout_ms;
    transfer->length = 0;
    transfer->actual_length = 0;
    transfer->callback = &State::on_transfer;
    transfer->user_data = &pending_transfer;
    transfer->buffer = nullptr;
    pending_transfer.phase = Phase::zero_packet;
}

void LibusbBulkOutBackend::State::cancel(const TransferId id) noexcept {
    std::lock_guard lock(mutex);
    const auto entry = pending.find(id);
    if (entry != pending.end()) {
        static_cast<void>(runtime->functions.cancel_transfer(entry->second->transfer));
    }
}

void LibusbBulkOutBackend::State::request_stop() noexcept {
    std::lock_guard lock(mutex);
    accepting = false;
    for (const auto& [ignored, pending_transfer] : pending) {
        static_cast<void>(ignored);
        static_cast<void>(runtime->functions.cancel_transfer(pending_transfer->transfer));
    }
    if (read_pending != nullptr) {
        static_cast<void>(runtime->functions.cancel_transfer(read_pending->transfer));
    }
}

void LibusbBulkOutBackend::State::service_raw(const bool shutting_down) {
    while (process_one_raw(shutting_down)) {
    }
    while (process_read_raw(shutting_down)) {
    }
}

bool LibusbBulkOutBackend::State::process_one_raw(const bool shutting_down) {
    std::lock_guard lock(mutex);
    if (completion_fifo_head == nullptr) {
        auto* completed = completion_head.exchange(nullptr, std::memory_order_acquire);
        while (completed != nullptr) {
            auto* next = completed->next_completed;
            completed->next_completed = completion_fifo_head;
            completion_fifo_head = completed;
            completed = next;
        }
    }
    if (completion_fifo_head == nullptr ||
        completion_fifo_head->raw_completion.event_epoch >
            runtime->completed_event_epoch.load(std::memory_order_acquire)) {
        return false;
    }

    auto* completed = completion_fifo_head;
    completion_fifo_head = completion_fifo_head->next_completed;
    completed->next_completed = nullptr;
    const auto raw = completed->raw_completion;
    const auto entry = pending.find(raw.id);
    if (entry == pending.end()) {
        return true;
    }
    auto& pending_transfer = *entry->second;
    const auto actual = raw.actual_length < 0 ? 0 : static_cast<std::size_t>(raw.actual_length);

    if (pending_transfer.phase == Phase::data &&
        raw.status == LIBUSB_TRANSFER_COMPLETED &&
        actual == pending_transfer.requested_bytes && pending_transfer.needs_zero_packet &&
        !shutting_down) {
        pending_transfer.data_transferred = actual;
        configure_zero_packet(pending_transfer);
        const auto result = runtime->functions.submit_transfer(pending_transfer.transfer);
        if (result == LIBUSB_SUCCESS) {
            return true;
        }
        finish_pending(entry,
                       result == LIBUSB_ERROR_NO_DEVICE ? CompletionCode::no_device
                                                        : CompletionCode::io_error,
                       pending_transfer.data_transferred);
        return true;
    }

    if (pending_transfer.phase == Phase::zero_packet) {
        const auto code = raw.status == LIBUSB_TRANSFER_COMPLETED && actual != 0
                              ? CompletionCode::io_error
                              : classify_completion(raw.status);
        finish_pending(entry, code, pending_transfer.data_transferred);
        return true;
    }

    auto code = classify_completion(raw.status);
    if (shutting_down && raw.status == LIBUSB_TRANSFER_COMPLETED &&
        pending_transfer.needs_zero_packet) {
        code = CompletionCode::cancelled;
    }
    finish_pending(entry, code, actual);
    return true;
}

void LibusbBulkOutBackend::State::finish_pending(
    std::unordered_map<TransferId, std::unique_ptr<Pending>>::iterator entry,
    const CompletionCode code,
    const std::size_t transferred_bytes) {
    const auto id = entry->first;
    auto finished = std::move(entry->second);
    pending.erase(entry);
    runtime->functions.free_transfer(finished->transfer);
    finished->transfer = nullptr;
    finished->payload_lifetime.reset();
    finished->fallback_payload = {};
    finished->ready_completion = TransferCompletion{id, code, transferred_bytes};
    finished->next_ready = nullptr;
    auto* ready = finished.release();
    if (ready_tail == nullptr) {
        ready_head = ready;
    } else {
        ready_tail->next_ready = ready;
    }
    ready_tail = ready;
}

void LibusbBulkOutBackend::State::configure_read_transfer(
    ReadPending& pending_read) noexcept {
    auto* transfer = pending_read.transfer;
    transfer->dev_handle = handle;
    transfer->flags = 0;
    transfer->endpoint = inbound_endpoint;
    transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
    transfer->timeout = libusb_timeout_until(pending_read.deadline);
    transfer->length = static_cast<int>(pending_read.buffer.size());
    transfer->actual_length = 0;
    transfer->callback = &State::on_read_transfer;
    transfer->user_data = &pending_read;
    transfer->buffer = reinterpret_cast<unsigned char*>(pending_read.buffer.data());
}

bool LibusbBulkOutBackend::State::process_read_raw(const bool shutting_down) {
    std::lock_guard lock(mutex);
    if (read_pending == nullptr ||
        !read_pending->raw_ready.load(std::memory_order_acquire) ||
        read_pending->raw_completion.event_epoch >
            runtime->completed_event_epoch.load(std::memory_order_acquire)) {
        return false;
    }

    read_pending->raw_ready.store(false, std::memory_order_release);
    const auto raw = read_pending->raw_completion;
    if (raw.actual_length < 0) {
        finish_read_locked(LibusbBulkOutBackend::ReadCode::io_error, 0, false);
        return true;
    }

    const auto actual = static_cast<std::size_t>(raw.actual_length);
    const auto copied = std::min(actual, read_pending->destination_capacity);
    if (raw.status == LIBUSB_TRANSFER_COMPLETED && actual == 0) {
        // A transfer-level ZLP terminates a preceding packet-aligned USB
        // transfer. It is not a Fastboot logical response by itself.
        if (shutting_down || !accepting) {
            finish_read_locked(LibusbBulkOutBackend::ReadCode::cancelled, 0, false);
            return true;
        }
        if (SteadyClock::now() >= read_pending->deadline) {
            finish_read_locked(LibusbBulkOutBackend::ReadCode::timeout, 0, false);
            return true;
        }
        configure_read_transfer(*read_pending);
        const auto result = runtime->functions.submit_transfer(read_pending->transfer);
        if (result == LIBUSB_SUCCESS) {
            return true;
        }
        finish_read_locked(
            result == LIBUSB_ERROR_NO_DEVICE
                ? LibusbBulkOutBackend::ReadCode::no_device
                : result == LIBUSB_ERROR_NO_MEM
                    ? LibusbBulkOutBackend::ReadCode::resource_exhausted
                    : LibusbBulkOutBackend::ReadCode::io_error,
            0,
            false);
        return true;
    }

    switch (raw.status) {
        case LIBUSB_TRANSFER_COMPLETED:
            if (actual >= read_pending->buffer.size()) {
                finish_read_locked(
                    LibusbBulkOutBackend::ReadCode::overflow, copied, true);
            } else {
                finish_read_locked(
                    LibusbBulkOutBackend::ReadCode::success, copied, false);
            }
            break;
        case LIBUSB_TRANSFER_TIMED_OUT:
            finish_read_locked(
                LibusbBulkOutBackend::ReadCode::timeout, copied, false);
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            finish_read_locked(
                LibusbBulkOutBackend::ReadCode::cancelled, copied, false);
            break;
        case LIBUSB_TRANSFER_STALL:
            finish_read_locked(
                LibusbBulkOutBackend::ReadCode::stall, copied, false);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            finish_read_locked(
                LibusbBulkOutBackend::ReadCode::no_device, copied, false);
            break;
        case LIBUSB_TRANSFER_OVERFLOW:
            finish_read_locked(
                LibusbBulkOutBackend::ReadCode::overflow, copied, true);
            break;
        case LIBUSB_TRANSFER_ERROR:
            finish_read_locked(
                LibusbBulkOutBackend::ReadCode::io_error, copied, false);
            break;
    }
    return true;
}

void LibusbBulkOutBackend::State::finish_read_locked(
    const LibusbBulkOutBackend::ReadCode code,
    const std::size_t transferred,
    const bool truncated) {
    auto finished = std::move(read_pending);
    runtime->functions.free_transfer(finished->transfer);
    finished->transfer = nullptr;
    ready_read.emplace(ReadyRead{
        LibusbBulkOutBackend::ReadResult{code, transferred, truncated},
        std::move(finished->buffer),
    });
}

std::optional<LibusbBulkOutBackend::State::ReadyRead>
LibusbBulkOutBackend::State::pop_ready_read() {
    std::lock_guard lock(mutex);
    if (!ready_read.has_value()) {
        return std::nullopt;
    }
    auto result = std::move(*ready_read);
    ready_read.reset();
    return result;
}

bool LibusbBulkOutBackend::State::service_shutdown() {
    service_raw(true);
    std::lock_guard lock(mutex);
    if (!pending.empty() || read_pending != nullptr) {
        return false;
    }
    close_if_drained();
    return true;
}

void LibusbBulkOutBackend::State::close_if_drained() noexcept {
    if (!closed && pending.empty() && read_pending == nullptr) {
        static_cast<void>(runtime->functions.release_interface(handle, interface_number));
        runtime->functions.close(handle);
        handle = nullptr;
        closed = true;
        release_reservation_locked();
    }
}

void LibusbBulkOutBackend::State::release_reservation_locked() noexcept {
    if (!reservation.has_value()) {
        return;
    }
    runtime->release_interface_reservation(*reservation);
    reservation.reset();
}

bool LibusbBulkOutBackend::State::try_pop(TransferCompletion& completion) {
    service_raw(false);
    std::lock_guard lock(mutex);
    if (ready_head == nullptr) {
        return false;
    }
    auto* ready = ready_head;
    ready_head = ready_head->next_ready;
    if (ready_head == nullptr) {
        ready_tail = nullptr;
    }
    completion = ready->ready_completion;
    delete ready;
    return true;
}

LibusbBulkOutBackend::WaitResult LibusbBulkOutBackend::State::wait_pop(
    const std::chrono::milliseconds timeout) {
    const auto deadline = deadline_after(timeout);
    for (;;) {
        const auto observed_epoch =
            runtime->completed_event_epoch.load(std::memory_order_acquire);
        TransferCompletion completion;
        if (try_pop(completion)) {
            return {LibusbBulkOutBackend::WaitCode::completion, completion};
        }
        if (runtime->event_terminal.load(std::memory_order_acquire)) {
            return {LibusbBulkOutBackend::WaitCode::event_error, {}};
        }
        {
            std::lock_guard lock(mutex);
            if (closed || quarantined.load(std::memory_order_acquire)) {
                return {LibusbBulkOutBackend::WaitCode::stopped, {}};
            }
        }
        if (SteadyClock::now() >= deadline) {
            return {LibusbBulkOutBackend::WaitCode::timeout, {}};
        }

        std::unique_lock completion_lock(runtime->completion_mutex);
        const auto predicate = [&] {
            return runtime->completed_event_epoch.load(std::memory_order_acquire) !=
                       observed_epoch ||
                   runtime->event_terminal.load(std::memory_order_acquire);
        };
        if (deadline == SteadyClock::time_point::max()) {
            runtime->completion_cv.wait(completion_lock, predicate);
        } else {
            static_cast<void>(runtime->completion_cv.wait_until(
                completion_lock, deadline, predicate));
        }
    }
}

LibusbBulkOutBackend::ReadResult
LibusbBulkOutBackend::State::read_logical_response(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    const auto deadline = deadline_after(timeout);
    if (SteadyClock::now() >= deadline) {
        return {LibusbBulkOutBackend::ReadCode::timeout, 0, false};
    }
    if (destination.size() >=
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {LibusbBulkOutBackend::ReadCode::io_error, 0, false};
    }

    {
        std::lock_guard lock(mutex);
        if (!accepting || closed ||
            !runtime->accepting.load(std::memory_order_acquire)) {
            return {LibusbBulkOutBackend::ReadCode::closed, 0, false};
        }
        if (read_pending != nullptr || ready_read.has_value()) {
            return {LibusbBulkOutBackend::ReadCode::io_error, 0, false};
        }

        libusb_transfer* transfer = nullptr;
        try {
            transfer = runtime->functions.alloc_transfer(0);
            if (transfer == nullptr) {
                return {LibusbBulkOutBackend::ReadCode::resource_exhausted, 0, false};
            }
            auto pending_read = std::make_unique<ReadPending>();
            pending_read->owner = this;
            pending_read->transfer = transfer;
            // The destination is never handed to libusb. This owned probe
            // buffer remains valid through a late callback or process-lifetime
            // quarantine, and its extra byte distinguishes a full destination
            // from a truncated logical response.
            pending_read->buffer.resize(destination.size() + 1U);
            pending_read->destination_capacity = destination.size();
            pending_read->deadline = deadline;
            configure_read_transfer(*pending_read);
            read_pending = std::move(pending_read);
            const auto result = runtime->functions.submit_transfer(transfer);
            if (result != LIBUSB_SUCCESS) {
                auto rejected = std::move(read_pending);
                runtime->functions.free_transfer(rejected->transfer);
                return {
                    result == LIBUSB_ERROR_NO_DEVICE
                        ? LibusbBulkOutBackend::ReadCode::no_device
                        : result == LIBUSB_ERROR_NO_MEM
                            ? LibusbBulkOutBackend::ReadCode::resource_exhausted
                            : LibusbBulkOutBackend::ReadCode::io_error,
                    0,
                    false,
                };
            }
        } catch (const std::bad_alloc&) {
            if (read_pending != nullptr) {
                runtime->functions.free_transfer(read_pending->transfer);
                read_pending.reset();
            } else if (transfer != nullptr) {
                runtime->functions.free_transfer(transfer);
            }
            return {LibusbBulkOutBackend::ReadCode::resource_exhausted, 0, false};
        } catch (...) {
            if (read_pending != nullptr) {
                runtime->functions.free_transfer(read_pending->transfer);
                read_pending.reset();
            } else if (transfer != nullptr) {
                runtime->functions.free_transfer(transfer);
            }
            return {LibusbBulkOutBackend::ReadCode::io_error, 0, false};
        }
    }

    for (;;) {
        const auto observed_epoch =
            runtime->completed_event_epoch.load(std::memory_order_acquire);
        service_raw(false);
        if (auto ready = pop_ready_read(); ready.has_value()) {
            if (ready->result.transferred != 0) {
                std::copy_n(ready->bytes.begin(),
                            ready->result.transferred,
                            destination.begin());
            }
            return ready->result;
        }
        if (runtime->event_terminal.load(std::memory_order_acquire)) {
            std::lock_guard lock(mutex);
            if (read_pending != nullptr) {
                static_cast<void>(
                    runtime->functions.cancel_transfer(read_pending->transfer));
            }
            return {LibusbBulkOutBackend::ReadCode::io_error, 0, false};
        }
        if (SteadyClock::now() >= deadline) {
            std::lock_guard lock(mutex);
            if (read_pending != nullptr) {
                static_cast<void>(
                    runtime->functions.cancel_transfer(read_pending->transfer));
            }
            return {LibusbBulkOutBackend::ReadCode::timeout, 0, false};
        }

        std::unique_lock completion_lock(runtime->completion_mutex);
        const auto predicate = [&] {
            return runtime->completed_event_epoch.load(std::memory_order_acquire) !=
                       observed_epoch ||
                   runtime->event_terminal.load(std::memory_order_acquire);
        };
        if (deadline == SteadyClock::time_point::max()) {
            runtime->completion_cv.wait(completion_lock, predicate);
        } else {
            static_cast<void>(runtime->completion_cv.wait_until(
                completion_lock, deadline, predicate));
        }
    }
}

std::size_t LibusbBulkOutBackend::State::in_flight() const noexcept {
    std::lock_guard lock(mutex);
    return pending.size() + (read_pending == nullptr ? 0U : 1U);
}

std::expected<std::shared_ptr<LibusbRuntime>, LibusbRuntimeError> LibusbRuntime::create(
    LibusbFunctions functions) {
    if (!functions.complete()) {
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::invalid_function_table});
    }

    const auto* version = functions.get_version();
    if (!version_is_exact(version)) {
        return std::unexpected(LibusbRuntimeError{
            LibusbRuntimeErrorKind::version_mismatch,
            0,
            version == nullptr ? std::uint16_t{0} : version->major,
            version == nullptr ? std::uint16_t{0} : version->minor,
            version == nullptr ? std::uint16_t{0} : version->micro,
        });
    }

    static std::mutex singleton_mutex;
    static std::weak_ptr<State> active_runtime;
    std::lock_guard singleton(singleton_mutex);
    if (const auto active = active_runtime.lock();
        active != nullptr &&
        (active->running.load(std::memory_order_acquire) ||
         active->quarantined.load(std::memory_order_acquire))) {
        return std::unexpected(LibusbRuntimeError{LibusbRuntimeErrorKind::already_running});
    }

    auto* const quarantine_root = &process_lifetime_quarantine_root();
    std::unique_ptr<std::shared_ptr<void>> quarantine_slot;
    try {
        quarantine_slot = std::make_unique<std::shared_ptr<void>>();
    } catch (const std::bad_alloc&) {
        return std::unexpected(LibusbRuntimeError{LibusbRuntimeErrorKind::init_failed,
                                                  LIBUSB_ERROR_NO_MEM});
    }

    libusb_context* context = nullptr;
    const auto init_result = functions.init(&context);
    if (init_result != LIBUSB_SUCCESS) {
        if (context != nullptr) {
            functions.exit(context);
        }
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::init_failed, init_result});
    }

    const auto exit_on_allocation_failure = functions.exit;
    std::shared_ptr<State> state;
    try {
        state = std::make_shared<State>(std::move(functions),
                                        context,
                                        quarantine_root,
                                        std::move(quarantine_slot));
    } catch (const std::bad_alloc&) {
        exit_on_allocation_failure(context);
        return std::unexpected(LibusbRuntimeError{LibusbRuntimeErrorKind::init_failed,
                                                  LIBUSB_ERROR_NO_MEM});
    }
    state->running.store(true, std::memory_order_release);
    state->accepting.store(true, std::memory_order_release);
    try {
        state->event_thread = std::thread([state] { state->event_loop(); });
    } catch (const std::system_error&) {
        state->functions.exit(context);
        state->context = nullptr;
        state->running.store(false, std::memory_order_release);
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::event_thread_failed});
    }

    // Publish the context before allocating the wrapper so even an allocation
    // failure followed by a quarantined shutdown cannot admit a second context.
    active_runtime = state;
    try {
        auto runtime = std::shared_ptr<LibusbRuntime>(new LibusbRuntime(state));
        return runtime;
    } catch (const std::bad_alloc&) {
        state->stop_all();
        return std::unexpected(LibusbRuntimeError{LibusbRuntimeErrorKind::init_failed,
                                                  LIBUSB_ERROR_NO_MEM});
    }
}

LibusbRuntime::LibusbRuntime(std::shared_ptr<State> state) : state_(std::move(state)) {}

LibusbRuntime::~LibusbRuntime() { stop(); }

bool LibusbRuntime::running() const noexcept {
    return state_ != nullptr && state_->running.load(std::memory_order_acquire);
}

void LibusbRuntime::stop() noexcept {
    if (state_ != nullptr) {
        state_->stop_all();
    }
}

std::expected<std::vector<UsbDeviceInfo>, LibusbRuntimeError> LibusbRuntime::enumerate(
    const UsbInterfaceFilter& filter) const {
    std::unique_lock lifecycle(state_->stop_mutex);
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::runtime_stopped});
    }

    libusb_device** list = nullptr;
    const auto count = state_->functions.get_device_list(state_->context, &list);
    if (count < 0 || (count != 0 && list == nullptr)) {
        return std::unexpected(LibusbRuntimeError{
            LibusbRuntimeErrorKind::enumeration_failed,
            count < 0 ? static_cast<int>(count) : LIBUSB_ERROR_OTHER});
    }
    DeviceListGuard list_guard{&state_->functions, list};
    std::vector<UsbDeviceInfo> devices;

    for (ssize_t index = 0; index < count; ++index) {
        auto* device = list[index];
        libusb_device_descriptor descriptor{};
        if (state_->functions.get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS ||
            !matches(filter.vendor_id, descriptor.idVendor) ||
            !matches(filter.product_id, descriptor.idProduct)) {
            continue;
        }

        libusb_config_descriptor* raw_config = nullptr;
        auto config_result =
            state_->functions.get_active_config_descriptor(device, &raw_config);
        if (config_result != LIBUSB_SUCCESS) {
            config_result = state_->functions.get_config_descriptor(device, 0, &raw_config);
        }
        if (config_result != LIBUSB_SUCCESS || raw_config == nullptr) {
            continue;
        }
        ConfigDescriptorGuard config_guard{&state_->functions, raw_config};
        if (raw_config->bConfigurationValue == 0) {
            continue;
        }

        std::optional<std::vector<std::uint8_t>> port_path;
        std::optional<std::string> serial;
        const auto load_identity = [&]() -> bool {
            if (port_path.has_value()) {
                return true;
            }

            std::array<std::uint8_t, 16> port_numbers{};
            const auto port_count = state_->functions.get_port_numbers(
                device, port_numbers.data(), static_cast<int>(port_numbers.size()));
            if (port_count <= 0 ||
                port_count > static_cast<int>(port_numbers.size())) {
                return false;
            }
            port_path.emplace(port_numbers.begin(),
                              port_numbers.begin() + port_count);

            // Descriptor string access can open/control-transfer the device on
            // some platforms. Do it only after a matching Fastboot interface
            // with one unambiguous bulk IN/OUT pair has been identified.
            std::array<char, LIBUSB_DEVICE_STRING_BYTES_MAX> serial_buffer{};
            const auto serial_result = state_->functions.get_device_string(
                device,
                LIBUSB_DEVICE_STRING_SERIAL_NUMBER,
                serial_buffer.data(),
                static_cast<int>(serial_buffer.size()));
            const auto serial_end =
                std::find(serial_buffer.begin(), serial_buffer.end(), '\0');
            serial.emplace(serial_result > 0
                               ? std::string(serial_buffer.begin(), serial_end)
                               : std::string{});
            return true;
        };

        for (std::uint8_t interface_index = 0;
             interface_index < raw_config->bNumInterfaces;
             ++interface_index) {
            if (raw_config->interface == nullptr) {
                break;
            }
            const auto& interface = raw_config->interface[interface_index];
            if (interface.num_altsetting > 0 && interface.altsetting == nullptr) {
                continue;
            }
            for (int alternate_index = 0;
                 alternate_index < interface.num_altsetting;
                 ++alternate_index) {
                const auto& alternate = interface.altsetting[alternate_index];
                if (!matches(filter.interface_class, alternate.bInterfaceClass) ||
                    !matches(filter.interface_subclass, alternate.bInterfaceSubClass) ||
                    !matches(filter.interface_protocol, alternate.bInterfaceProtocol)) {
                    continue;
                }
                if (alternate.bNumEndpoints != 0 && alternate.endpoint == nullptr) {
                    continue;
                }
                const libusb_endpoint_descriptor* bulk_out = nullptr;
                const libusb_endpoint_descriptor* bulk_in = nullptr;
                bool ambiguous_bulk_pair = false;
                for (std::uint8_t endpoint_index = 0;
                     endpoint_index < alternate.bNumEndpoints;
                     ++endpoint_index) {
                    const auto& endpoint_descriptor = alternate.endpoint[endpoint_index];
                    const auto transfer_type =
                        endpoint_descriptor.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                    const auto direction =
                        endpoint_descriptor.bEndpointAddress & LIBUSB_ENDPOINT_IN;
                    if (transfer_type != LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK) {
                        continue;
                    }
                    auto*& selected = direction == LIBUSB_ENDPOINT_IN
                                          ? bulk_in
                                          : bulk_out;
                    if (selected != nullptr) {
                        ambiguous_bulk_pair = true;
                    } else {
                        selected = &endpoint_descriptor;
                    }
                }
                if (ambiguous_bulk_pair || bulk_out == nullptr || bulk_in == nullptr ||
                    !load_identity()) {
                    continue;
                }
                devices.push_back(UsbDeviceInfo{
                    descriptor.idVendor,
                    descriptor.idProduct,
                    state_->functions.get_bus_number(device),
                    state_->functions.get_device_address(device),
                    raw_config->bConfigurationValue,
                    *port_path,
                    *serial,
                    alternate.bInterfaceNumber,
                    alternate.bAlternateSetting,
                    alternate.bInterfaceClass,
                    alternate.bInterfaceSubClass,
                    alternate.bInterfaceProtocol,
                    bulk_out->bEndpointAddress,
                    static_cast<std::uint16_t>(bulk_out->wMaxPacketSize & 0x07FFU),
                    bulk_in->bEndpointAddress,
                    static_cast<std::uint16_t>(bulk_in->wMaxPacketSize & 0x07FFU),
                });
            }
        }
    }
    return devices;
}

std::expected<std::unique_ptr<LibusbBulkOutBackend>, LibusbRuntimeError>
LibusbRuntime::open_bulk_out(const UsbDeviceInfo& device, const BulkOutOptions options) {
    std::unique_lock lifecycle(state_->stop_mutex);
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::runtime_stopped});
    }
    if ((device.bulk_out_endpoint & LIBUSB_ENDPOINT_IN) != LIBUSB_ENDPOINT_OUT ||
        (device.bulk_in_endpoint & LIBUSB_ENDPOINT_IN) != LIBUSB_ENDPOINT_IN ||
        device.configuration_value == 0 ||
        device.bulk_out_max_packet_size == 0 ||
        device.bulk_in_max_packet_size == 0 || device.port_path.empty() ||
        device.port_path.size() > 16) {
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::invalid_device});
    }

    auto reservation_result = state_->reserve_interface(device);
    if (!reservation_result.has_value()) {
        return std::unexpected(reservation_result.error());
    }
    auto reserved_key = std::move(*reservation_result);
    bool reservation_owned_by_open = true;
    const auto release_open_reservation = [&]() noexcept {
        if (reservation_owned_by_open) {
            state_->release_interface_reservation(reserved_key);
            reservation_owned_by_open = false;
        }
    };

    libusb_device** list = nullptr;
    const auto count = state_->functions.get_device_list(state_->context, &list);
    if (count < 0 || (count != 0 && list == nullptr)) {
        release_open_reservation();
        return std::unexpected(LibusbRuntimeError{
            LibusbRuntimeErrorKind::enumeration_failed,
            count < 0 ? static_cast<int>(count) : LIBUSB_ERROR_OTHER});
    }
    DeviceListGuard list_guard{&state_->functions, list};

    libusb_device_handle* handle = nullptr;
    for (ssize_t index = 0; index < count; ++index) {
        auto* candidate = list[index];
        // USB addresses can be reused across re-enumeration. The stable key is
        // bus + physical port path, with serial (when present) and the complete
        // interface/endpoint snapshot revalidated immediately before open.
        if (!open_snapshot_matches(state_->functions, candidate, device)) {
            continue;
        }
        const auto open_result = state_->functions.open(candidate, &handle);
        if (open_result != LIBUSB_SUCCESS) {
            release_open_reservation();
            return std::unexpected(
                LibusbRuntimeError{LibusbRuntimeErrorKind::open_failed, open_result});
        }
        break;
    }
    if (handle == nullptr) {
        release_open_reservation();
        return std::unexpected(
            LibusbRuntimeError{LibusbRuntimeErrorKind::device_not_found});
    }

    int active_configuration = 0;
    const auto configuration_result =
        state_->functions.get_configuration(handle, &active_configuration);
    if (configuration_result != LIBUSB_SUCCESS) {
        state_->functions.close(handle);
        release_open_reservation();
        return std::unexpected(LibusbRuntimeError{
            LibusbRuntimeErrorKind::configuration_failed,
            configuration_result,
        });
    }
    if (active_configuration != device.configuration_value) {
        const auto set_result = state_->functions.set_configuration(
            handle, device.configuration_value);
        if (set_result != LIBUSB_SUCCESS) {
            state_->functions.close(handle);
            release_open_reservation();
            return std::unexpected(LibusbRuntimeError{
                LibusbRuntimeErrorKind::configuration_failed,
                set_result,
            });
        }
    }

    const auto claim_result =
        state_->functions.claim_interface(handle, device.interface_number);
    if (claim_result != LIBUSB_SUCCESS) {
        state_->functions.close(handle);
        release_open_reservation();
        return std::unexpected(LibusbRuntimeError{
            claim_result == LIBUSB_ERROR_BUSY
                ? LibusbRuntimeErrorKind::interface_busy
                : LibusbRuntimeErrorKind::claim_failed,
            claim_result,
        });
    }
    if (device.alternate_setting != 0) {
        const auto alternate_result = state_->functions.set_interface_alt_setting(
            handle, device.interface_number, device.alternate_setting);
        if (alternate_result != LIBUSB_SUCCESS) {
            static_cast<void>(
                state_->functions.release_interface(handle, device.interface_number));
            state_->functions.close(handle);
            release_open_reservation();
            return std::unexpected(LibusbRuntimeError{
                LibusbRuntimeErrorKind::alternate_setting_failed, alternate_result});
        }
    }

    std::shared_ptr<LibusbBulkOutBackend::State> backend_state;
    try {
        backend_state =
            std::make_shared<LibusbBulkOutBackend::State>(
                state_, handle, device, options, reserved_key);
        reservation_owned_by_open = false;
        state_->register_backend(backend_state);
        return std::unique_ptr<LibusbBulkOutBackend>(
            new LibusbBulkOutBackend(backend_state));
    } catch (const std::bad_alloc&) {
        if (backend_state != nullptr) {
            lifecycle.unlock();
            state_->stop_backend(backend_state);
        } else {
            static_cast<void>(
                state_->functions.release_interface(handle, device.interface_number));
            state_->functions.close(handle);
            release_open_reservation();
        }
        return std::unexpected(LibusbRuntimeError{LibusbRuntimeErrorKind::open_failed,
                                                  LIBUSB_ERROR_NO_MEM});
    }
}

std::optional<int> LibusbRuntime::last_event_error() const noexcept {
    if (state_ == nullptr) {
        return std::nullopt;
    }
    const auto error = state_->event_error.load(std::memory_order_acquire);
    return error == 0 ? std::nullopt : std::optional<int>{error};
}

std::thread::id LibusbRuntime::event_thread_id() const noexcept {
    if (state_ == nullptr) {
        return {};
    }
    std::lock_guard lock(state_->event_identity_mutex);
    return state_->event_identity;
}

bool LibusbRuntime::shutdown_quarantined() const noexcept {
    return state_ != nullptr && state_->quarantined.load(std::memory_order_acquire);
}

bool LibusbRuntime::quarantine_module_pin_failed() const noexcept {
    return state_ != nullptr &&
           state_->module_pin_failed.load(std::memory_order_acquire);
}

LibusbBulkOutBackend::LibusbBulkOutBackend(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

LibusbBulkOutBackend::~LibusbBulkOutBackend() { stop(); }

SubmitResult LibusbBulkOutBackend::submit(
    const TransferSubmission& submission) noexcept {
    return state_->submit(submission);
}

void LibusbBulkOutBackend::cancel(const TransferId id) noexcept { state_->cancel(id); }

bool LibusbBulkOutBackend::try_pop_completion(TransferCompletion& completion) {
    return state_->try_pop(completion);
}

LibusbBulkOutBackend::WaitResult LibusbBulkOutBackend::wait_for_completion(
    const std::chrono::milliseconds timeout) {
    return state_->wait_pop(timeout);
}

LibusbBulkOutBackend::ReadResult LibusbBulkOutBackend::read_logical_response(
    const std::span<std::byte> destination,
    const std::chrono::milliseconds timeout) {
    return state_->read_logical_response(destination, timeout);
}

std::size_t LibusbBulkOutBackend::in_flight() const noexcept {
    return state_->in_flight();
}

bool LibusbBulkOutBackend::shutdown_quarantined() const noexcept {
    return state_ != nullptr && state_->quarantined.load(std::memory_order_acquire);
}

void LibusbBulkOutBackend::stop() noexcept {
    if (state_ != nullptr) {
        state_->runtime->stop_backend(state_);
    }
}

}  // namespace kairosboot::transport
