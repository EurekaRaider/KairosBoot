#include "src/transport/libusb_runtime.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using kairosboot::transport::BulkOutOptions;
using kairosboot::transport::BufferBudget;
using kairosboot::transport::CompletionCode;
using kairosboot::transport::LibusbBulkOutBackend;
using kairosboot::transport::LibusbFunctions;
using kairosboot::transport::LibusbRuntime;
using kairosboot::transport::LibusbRuntimeErrorKind;
using kairosboot::transport::LibusbSubmitFaultPoint;
using kairosboot::transport::LinuxUsbTopology;
using kairosboot::transport::LinuxUsbTopologyError;
using kairosboot::transport::LinuxUsbTopologyErrorKind;
using kairosboot::transport::LinuxUsbTopologyQuery;
using kairosboot::transport::LinuxUsbTopologyStage;
using kairosboot::transport::MacUsbTopology;
using kairosboot::transport::MacUsbTopologyError;
using kairosboot::transport::MacUsbTopologyErrorKind;
using kairosboot::transport::MacUsbTopologyQuery;
using kairosboot::transport::MacUsbTopologyStage;
using kairosboot::transport::MemoryTransferSource;
using kairosboot::transport::SubmitResult;
using kairosboot::transport::TransferCompletion;
using kairosboot::transport::TransferId;
using kairosboot::transport::TransferRing;
using kairosboot::transport::TransferRingConfig;
using kairosboot::transport::TransferRingState;
using kairosboot::transport::TransferProgressAction;
using kairosboot::transport::TransferSource;
using kairosboot::transport::TransferSubmission;
using kairosboot::transport::TransferTelemetryConfig;
using kairosboot::transport::TransferTelemetryTimePoint;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::UsbFastbootTransport;
using kairosboot::transport::UsbFastbootTransportOptions;
using kairosboot::transport::UsbInterfaceFilter;
using kairosboot::transport::WindowsUsbNativeErrorDomain;
using kairosboot::transport::WindowsUsbTopology;
using kairosboot::transport::WindowsUsbTopologyError;
using kairosboot::transport::WindowsUsbTopologyErrorKind;
using kairosboot::transport::WindowsUsbTopologyQuery;
using kairosboot::transport::WindowsUsbTopologyStage;
using kairosboot::transport::ZeroPacketPolicy;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        throw TestFailure(std::string("line ") + std::to_string(line) + ": " +
                          std::string(expression));
    }
}

#define KB_CHECK(expression) check((expression), #expression, __LINE__)

template <typename Predicate>
void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw TestFailure("timed out waiting for deterministic fake event");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct IncrementingTelemetryClock final {
    TransferTelemetryTimePoint current{};
    std::chrono::nanoseconds step{1};
    std::atomic<std::size_t> calls{0};
    std::size_t block_on_call{};
    std::atomic<bool> blocked{false};
    std::atomic<bool> resume{false};
    std::thread::id caller_thread;
};

[[nodiscard]] TransferTelemetryTimePoint sample_telemetry_clock(
    void* const context) noexcept {
    auto& clock = *static_cast<IncrementingTelemetryClock*>(context);
    const auto sampled = clock.current;
    clock.current += clock.step;
    const auto call = clock.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    clock.caller_thread = std::this_thread::get_id();
    if (call == clock.block_on_call) {
        clock.blocked.store(true, std::memory_order_release);
        while (!clock.resume.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    return sampled;
}

class FakeLibusb final : public std::enable_shared_from_this<FakeLibusb> {
public:
    struct Event final {
        libusb_transfer* transfer{};
        libusb_transfer_status status{LIBUSB_TRANSFER_ERROR};
        int actual_length{};
    };

    struct SubmissionSnapshot final {
        std::uint8_t endpoint{};
        const unsigned char* buffer{};
        int length{};
        unsigned int timeout{};
        std::uint8_t flags{};
    };

    FakeLibusb() {
        version_.major = 1;
        version_.minor = 0;
        version_.micro = 30;
        descriptor_.idVendor = 0x18D1;
        descriptor_.idProduct = 0x4EE0;
        descriptor_.iSerialNumber = 1;
        descriptor_.bNumConfigurations = 1;

        endpoints_[0].bEndpointAddress = 0x01;
        endpoints_[0].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
        endpoints_[0].wMaxPacketSize = 4;
        endpoints_[1].bEndpointAddress = 0x81;
        endpoints_[1].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
        endpoints_[1].wMaxPacketSize = 4;

        alternate_.bInterfaceNumber = 2;
        alternate_.bAlternateSetting = 0;
        alternate_.bNumEndpoints = 2;
        alternate_.bInterfaceClass = 0xFF;
        alternate_.bInterfaceSubClass = 0x42;
        alternate_.bInterfaceProtocol = 0x03;
        alternate_.endpoint = endpoints_;
        interface_.altsetting = &alternate_;
        interface_.num_altsetting = 1;
        config_.bConfigurationValue = 1;
        config_.bNumInterfaces = 1;
        config_.interface = &interface_;
        device_list_[0] = device();
        device_list_[1] = nullptr;
    }

    [[nodiscard]] LibusbFunctions functions() {
        const auto self = shared_from_this();
        LibusbFunctions table;
        table.get_version = [self] { return &self->version_; };
        table.init = [self](libusb_context** context) -> int {
            ++self->init_calls;
            if (self->init_result != LIBUSB_SUCCESS) {
                *context = nullptr;
                return self->init_result;
            }
            *context = self->context();
            return LIBUSB_SUCCESS;
        };
        table.exit = [self](libusb_context*) { ++self->exit_calls; };
        table.handle_events = [self](libusb_context*, timeval* timeout, int*) {
            return self->handle_one_event(timeout);
        };
        table.interrupt_events = [self](libusb_context*) {
            {
                std::lock_guard lock(self->mutex_);
                self->interrupted_ = true;
            }
            self->event_cv_.notify_all();
        };
        table.get_device_list = [self](libusb_context*, libusb_device*** list) {
            *list = self->device_list_;
            return static_cast<ssize_t>(1);
        };
        table.free_device_list = [self](libusb_device**, int) {
            ++self->free_device_list_calls;
        };
        table.get_device_descriptor = [self](libusb_device*,
                                             libusb_device_descriptor* descriptor) {
            *descriptor = self->descriptor_;
            return LIBUSB_SUCCESS;
        };
        table.get_active_config_descriptor = [self](libusb_device*,
                                                    libusb_config_descriptor** config) -> int {
            if (self->active_config_result != LIBUSB_SUCCESS) {
                *config = nullptr;
                return self->active_config_result;
            }
            *config = &self->config_;
            return LIBUSB_SUCCESS;
        };
        table.get_config_descriptor = [self](libusb_device*,
                                             std::uint8_t,
                                             libusb_config_descriptor** config) -> int {
            if (self->config_result != LIBUSB_SUCCESS) {
                *config = nullptr;
                return self->config_result;
            }
            *config = &self->config_;
            return LIBUSB_SUCCESS;
        };
        table.free_config_descriptor = [self](libusb_config_descriptor*) {
            ++self->free_config_calls;
        };
        table.get_bus_number = [](libusb_device*) { return std::uint8_t{2}; };
        table.get_device_address = [](libusb_device*) { return std::uint8_t{5}; };
        table.get_session_data = [self](libusb_device*) {
            ++self->session_data_calls;
            return self->session_data;
        };
        table.get_port_numbers = [](libusb_device*,
                                    std::uint8_t* path,
                                    int length) -> int {
            if (length < 2) {
                return LIBUSB_ERROR_OVERFLOW;
            }
            path[0] = 3;
            path[1] = 4;
            return 2;
        };
        table.get_device_string = [self](libusb_device*,
                                         libusb_device_string_type,
                                         char* output,
                                         int length) -> int {
            ++self->serial_calls;
            if (length <= static_cast<int>(self->serial_.size())) {
                return LIBUSB_ERROR_OVERFLOW;
            }
            std::memcpy(output, self->serial_.c_str(), self->serial_.size() + 1U);
            return static_cast<int>(self->serial_.size() + 1U);
        };
        table.open = [self](libusb_device*, libusb_device_handle** handle) -> int {
            ++self->open_calls;
            if (self->open_result != LIBUSB_SUCCESS) {
                *handle = nullptr;
                return self->open_result;
            }
            *handle = self->handle();
            return LIBUSB_SUCCESS;
        };
        table.close = [self](libusb_device_handle*) { ++self->close_calls; };
        table.get_configuration = [self](libusb_device_handle*, int* configuration) {
            self->get_configuration_order = ++self->open_sequence;
            ++self->get_configuration_calls;
            if (self->get_configuration_result == LIBUSB_SUCCESS) {
                *configuration = self->current_configuration;
            }
            return self->get_configuration_result;
        };
        table.set_configuration = [self](libusb_device_handle*, int configuration) {
            self->set_configuration_order = ++self->open_sequence;
            ++self->set_configuration_calls;
            self->last_set_configuration = configuration;
            if (self->set_configuration_result == LIBUSB_SUCCESS) {
                self->current_configuration = configuration;
            }
            return self->set_configuration_result;
        };
        table.claim_interface = [self](libusb_device_handle*, int) {
            self->claim_order = ++self->open_sequence;
            ++self->claim_calls;
            return self->claim_result;
        };
        table.release_interface = [self](libusb_device_handle*, int) {
            ++self->release_calls;
            return LIBUSB_SUCCESS;
        };
        table.set_interface_alt_setting = [self](libusb_device_handle*, int, int) {
            ++self->alternate_calls;
            return self->alternate_result;
        };
        table.alloc_transfer = [self](int) {
            if (self->fail_allocation) {
                return static_cast<libusb_transfer*>(nullptr);
            }
            auto* transfer = static_cast<libusb_transfer*>(
                std::calloc(1, sizeof(libusb_transfer)));
            return transfer;
        };
        table.submit_transfer = [self](libusb_transfer* transfer) {
            std::lock_guard lock(self->mutex_);
            int result = LIBUSB_SUCCESS;
            if (!self->submit_results_.empty()) {
                result = self->submit_results_.front();
                self->submit_results_.pop_front();
            }
            if (result == LIBUSB_SUCCESS) {
                self->submissions_.push_back(transfer);
                self->active_.insert(transfer);
            }
            return result;
        };
        table.cancel_transfer = [self](libusb_transfer* transfer) -> int {
            std::lock_guard lock(self->mutex_);
            ++self->cancel_calls;
            if (!self->active_.contains(transfer) ||
                self->cancel_queued_.contains(transfer)) {
                return LIBUSB_ERROR_NOT_FOUND;
            }
            self->cancel_queued_.insert(transfer);
            const auto inbound =
                (transfer->endpoint & LIBUSB_ENDPOINT_IN) == LIBUSB_ENDPOINT_IN;
            if (self->suppress_cancel_completion ||
                (inbound && self->suppress_in_cancel_completion)) {
                return LIBUSB_SUCCESS;
            }
            self->events_.push_back(
                Event{transfer, LIBUSB_TRANSFER_CANCELLED, self->cancel_actual_length});
            self->event_cv_.notify_all();
            return LIBUSB_SUCCESS;
        };
        table.free_transfer = [self](libusb_transfer* transfer) {
            ++self->free_transfer_calls;
            std::free(transfer);
        };
        table.pin_current_module = [self] {
            ++self->pin_module_calls;
            return self->pin_module_result;
        };
        table.module_pin_failure = [self] { ++self->pin_module_failure_calls; };
        return table;
    }

    void queue_submit_result(const int result) {
        std::lock_guard lock(mutex_);
        submit_results_.push_back(result);
    }

    void queue_event_result(const int result) {
        std::lock_guard lock(mutex_);
        event_results_.push_back(result);
        event_cv_.notify_all();
    }

    void request_event_block() {
        std::lock_guard lock(mutex_);
        event_block_requested_ = true;
        event_block_released_ = false;
        blocked_event_started.store(false, std::memory_order_release);
        blocked_event_completed.store(false, std::memory_order_release);
        event_cv_.notify_all();
    }

    void wait_for_event_block_start() {
        wait_until([this] {
            return blocked_event_started.load(std::memory_order_acquire);
        });
    }

    void release_event_block() {
        {
            std::lock_guard lock(mutex_);
            event_block_released_ = true;
        }
        event_cv_.notify_all();
    }

    void complete_submission(const std::size_t index,
                             const libusb_transfer_status status,
                             const int actual_length) {
        std::lock_guard lock(mutex_);
        if (index >= submissions_.size()) {
            throw TestFailure("fake submission index out of range");
        }
        events_.push_back(Event{submissions_[index], status, actual_length});
        event_cv_.notify_all();
    }

    void complete_in_submission(const std::size_t index,
                                const libusb_transfer_status status,
                                const std::span<const std::byte> bytes,
                                const std::optional<int> actual_length = std::nullopt) {
        std::lock_guard lock(mutex_);
        if (index >= submissions_.size()) {
            throw TestFailure("fake submission index out of range");
        }
        auto* transfer = submissions_[index];
        if ((transfer->endpoint & LIBUSB_ENDPOINT_IN) != LIBUSB_ENDPOINT_IN) {
            throw TestFailure("fake inbound completion targeted a bulk OUT transfer");
        }
        const auto writable = transfer->length < 0
            ? std::size_t{0}
            : static_cast<std::size_t>(transfer->length);
        const auto copied = std::min(bytes.size(), writable);
        if (copied != 0) {
            std::memcpy(transfer->buffer, bytes.data(), copied);
        }
        events_.push_back(Event{
            transfer,
            status,
            actual_length.value_or(static_cast<int>(bytes.size())),
        });
        event_cv_.notify_all();
    }

    [[nodiscard]] std::size_t submission_count() const {
        std::lock_guard lock(mutex_);
        return submissions_.size();
    }

    [[nodiscard]] SubmissionSnapshot submission(const std::size_t index) const {
        std::lock_guard lock(mutex_);
        const auto* transfer = submissions_.at(index);
        return SubmissionSnapshot{
            transfer->endpoint,
            transfer->buffer,
            transfer->length,
            transfer->timeout,
            transfer->flags,
        };
    }

    void wait_for_event_loop() {
        wait_until([this] { return handle_event_calls.load() != 0; });
    }

    libusb_version version_{};
    int init_result{LIBUSB_SUCCESS};
    int active_config_result{LIBUSB_SUCCESS};
    int config_result{LIBUSB_SUCCESS};
    int open_result{LIBUSB_SUCCESS};
    int get_configuration_result{LIBUSB_SUCCESS};
    int set_configuration_result{LIBUSB_SUCCESS};
    int current_configuration{1};
    int last_set_configuration{-1};
    unsigned long session_data{0x101UL};
    int claim_result{LIBUSB_SUCCESS};
    int alternate_result{LIBUSB_SUCCESS};
    int cancel_actual_length{};
    bool fail_allocation{false};
    bool suppress_cancel_completion{false};
    bool suppress_in_cancel_completion{false};
    bool pin_module_result{true};
    std::atomic<bool> blocked_event_started{false};
    std::atomic<bool> blocked_event_completed{false};
    std::atomic<int> init_calls{0};
    std::atomic<int> exit_calls{0};
    std::atomic<int> handle_event_calls{0};
    std::atomic<int> free_device_list_calls{0};
    std::atomic<int> free_config_calls{0};
    std::atomic<int> serial_calls{0};
    std::atomic<int> session_data_calls{0};
    std::atomic<int> open_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> get_configuration_calls{0};
    std::atomic<int> set_configuration_calls{0};
    std::atomic<int> claim_calls{0};
    std::atomic<int> release_calls{0};
    std::atomic<int> alternate_calls{0};
    std::atomic<int> cancel_calls{0};
    std::atomic<int> free_transfer_calls{0};
    std::atomic<int> pin_module_calls{0};
    std::atomic<int> pin_module_failure_calls{0};
    std::atomic<int> open_sequence{0};
    std::atomic<int> get_configuration_order{0};
    std::atomic<int> set_configuration_order{0};
    std::atomic<int> claim_order{0};
    std::thread::id callback_thread;

private:
    [[nodiscard]] libusb_context* context() noexcept {
        return reinterpret_cast<libusb_context*>(&context_storage_);
    }
    [[nodiscard]] libusb_device* device() noexcept {
        return reinterpret_cast<libusb_device*>(&device_storage_);
    }
    [[nodiscard]] libusb_device_handle* handle() noexcept {
        return reinterpret_cast<libusb_device_handle*>(&handle_storage_);
    }

    int handle_one_event(timeval* timeout) {
        ++handle_event_calls;
        std::unique_lock lock(mutex_);
        const auto wait_duration = timeout == nullptr
                                       ? std::chrono::milliseconds(100)
                                       : std::chrono::seconds(timeout->tv_sec) +
                                             std::chrono::microseconds(timeout->tv_usec);
        event_cv_.wait_for(lock, wait_duration, [this] {
            return interrupted_ || !events_.empty() || !event_results_.empty() ||
                   event_block_requested_;
        });
        if (event_block_requested_) {
            event_block_requested_ = false;
            blocked_event_started.store(true, std::memory_order_release);
            event_cv_.notify_all();
            event_cv_.wait(lock, [this] { return event_block_released_; });
            blocked_event_completed.store(true, std::memory_order_release);
            event_cv_.notify_all();
            return LIBUSB_SUCCESS;
        }
        if (interrupted_) {
            interrupted_ = false;
            return LIBUSB_ERROR_INTERRUPTED;
        }
        if (!event_results_.empty()) {
            const auto result = event_results_.front();
            event_results_.pop_front();
            return result;
        }
        if (events_.empty()) {
            return LIBUSB_SUCCESS;
        }

        const auto event = events_.front();
        events_.pop_front();
        active_.erase(event.transfer);
        cancel_queued_.erase(event.transfer);
        lock.unlock();

        event.transfer->status = event.status;
        event.transfer->actual_length = event.actual_length;
        callback_thread = std::this_thread::get_id();
        event.transfer->callback(event.transfer);
        return LIBUSB_SUCCESS;
    }

    std::byte context_storage_{};
    std::byte device_storage_{};
    std::byte handle_storage_{};
    libusb_device_descriptor descriptor_{};
    libusb_endpoint_descriptor endpoints_[2]{};
    libusb_interface_descriptor alternate_{};
    libusb_interface interface_{};
    libusb_config_descriptor config_{};
    libusb_device* device_list_[2]{};
    std::string serial_{"serial-\xCE\xB1"};
    mutable std::mutex mutex_;
    std::condition_variable event_cv_;
    bool interrupted_{false};
    bool event_block_requested_{false};
    bool event_block_released_{false};
    std::deque<Event> events_;
    std::deque<int> event_results_;
    std::deque<int> submit_results_;
    std::vector<libusb_transfer*> submissions_;
    std::unordered_set<libusb_transfer*> active_;
    std::unordered_set<libusb_transfer*> cancel_queued_;
};

[[nodiscard]] std::shared_ptr<LibusbRuntime> create_runtime(
    const std::shared_ptr<FakeLibusb>& fake,
    LibusbFunctions functions) {
    auto runtime = LibusbRuntime::create(std::move(functions));
    KB_CHECK(runtime.has_value());
    fake->wait_for_event_loop();
    return *runtime;
}

[[nodiscard]] std::shared_ptr<LibusbRuntime> create_runtime(
    const std::shared_ptr<FakeLibusb>& fake) {
    return create_runtime(fake, fake->functions());
}

[[nodiscard]] UsbDeviceInfo matching_device(const std::shared_ptr<LibusbRuntime>& runtime) {
    UsbInterfaceFilter filter;
    filter.vendor_id = 0x18D1;
    filter.product_id = 0x4EE0;
    filter.interface_class = 0xFF;
    filter.interface_subclass = 0x42;
    filter.interface_protocol = 0x03;
    auto devices = runtime->enumerate(filter);
    KB_CHECK(devices.has_value());
    KB_CHECK(devices->size() == 1);
    return devices->front();
}

[[nodiscard]] TransferCompletion wait_for_completion(LibusbBulkOutBackend& backend) {
    TransferCompletion completion;
    wait_until([&] { return backend.try_pop_completion(completion); });
    return completion;
}

[[nodiscard]] std::shared_ptr<std::vector<std::byte>> payload(const std::size_t size) {
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    for (std::size_t index = 0; index < size; ++index) {
        (*bytes)[index] = std::byte{static_cast<unsigned char>(index & 0xFFU)};
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> ascii_bytes(const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const auto character : text) {
        bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return bytes;
}

void wait_for_submissions(const FakeLibusb& fake, const std::size_t count) {
    wait_until([&] { return fake.submission_count() >= count; });
}

class PatternTransferSource final : public TransferSource {
public:
    struct Read final {
        std::uint64_t offset{};
        std::size_t bytes{};

        [[nodiscard]] bool operator==(const Read&) const = default;
    };

    explicit PatternTransferSource(
        const std::uint64_t size,
        const std::uint64_t fail_offset = std::numeric_limits<std::uint64_t>::max())
        : size_(size), fail_offset_(fail_offset) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return size_;
    }

    [[nodiscard]] bool read_exact(
        const std::uint64_t offset,
        const std::span<std::byte> destination) noexcept override {
        if (read_count_ >= reads_.size()) {
            return false;
        }
        reads_[read_count_++] = Read{offset, destination.size()};
        if (offset == fail_offset_ || offset > size_ ||
            destination.size() > size_ - offset) {
            return false;
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = std::byte{static_cast<unsigned char>(
                (offset + index) & 0xFFU)};
        }
        return true;
    }

    [[nodiscard]] std::span<const Read> reads() const noexcept {
        return std::span<const Read>{reads_.data(), read_count_};
    }

private:
    std::uint64_t size_{};
    std::uint64_t fail_offset_{};
    std::array<Read, 32> reads_{};
    std::size_t read_count_{};
};

void test_init_failure_version_and_singleton() {
    const auto system_functions = LibusbFunctions::system();
    KB_CHECK(system_functions.complete());
    KB_CHECK(system_functions.pin_current_module());
    const auto* linked_version = system_functions.get_version();
    KB_CHECK(linked_version != nullptr);
    KB_CHECK(linked_version->major == 1);
    KB_CHECK(linked_version->minor == 0);
    KB_CHECK(linked_version->micro == 30);

    const auto incomplete = LibusbRuntime::create(LibusbFunctions{});
    KB_CHECK(!incomplete.has_value());
    KB_CHECK(incomplete.error().kind == LibusbRuntimeErrorKind::invalid_function_table);

    auto missing_session = std::make_shared<FakeLibusb>();
    auto missing_session_functions = missing_session->functions();
    missing_session_functions.get_session_data = {};
    const auto missing_session_result =
        LibusbRuntime::create(std::move(missing_session_functions));
    KB_CHECK(!missing_session_result.has_value());
    KB_CHECK(missing_session_result.error().kind ==
             LibusbRuntimeErrorKind::invalid_function_table);
    KB_CHECK(missing_session->init_calls == 0);

    auto missing_windows_identity = std::make_shared<FakeLibusb>();
    auto missing_windows_identity_functions =
        missing_windows_identity->functions();
    missing_windows_identity_functions.resolve_windows_topology =
        [](const WindowsUsbTopologyQuery&) {
            return std::expected<WindowsUsbTopology,
                                 WindowsUsbTopologyError>{
                std::unexpected(WindowsUsbTopologyError{})};
        };
    const auto missing_windows_identity_result = LibusbRuntime::create(
        std::move(missing_windows_identity_functions));
    KB_CHECK(!missing_windows_identity_result.has_value());
    KB_CHECK(missing_windows_identity_result.error().kind ==
             LibusbRuntimeErrorKind::invalid_function_table);
    KB_CHECK(missing_windows_identity->init_calls == 0);

    auto wrong_version = std::make_shared<FakeLibusb>();
    wrong_version->version_.micro = 29;
    const auto version_result = LibusbRuntime::create(wrong_version->functions());
    KB_CHECK(!version_result.has_value());
    KB_CHECK(version_result.error().kind == LibusbRuntimeErrorKind::version_mismatch);
    KB_CHECK(wrong_version->init_calls == 0);

    auto release_candidate = std::make_shared<FakeLibusb>();
    release_candidate->version_.rc = "-rc1";
    const auto release_candidate_result =
        LibusbRuntime::create(release_candidate->functions());
    KB_CHECK(!release_candidate_result.has_value());
    KB_CHECK(release_candidate_result.error().kind ==
             LibusbRuntimeErrorKind::version_mismatch);

    auto init_failure = std::make_shared<FakeLibusb>();
    init_failure->init_result = LIBUSB_ERROR_NO_MEM;
    const auto init_result = LibusbRuntime::create(init_failure->functions());
    KB_CHECK(!init_result.has_value());
    KB_CHECK(init_result.error().kind == LibusbRuntimeErrorKind::init_failed);
    KB_CHECK(init_result.error().native_code == LIBUSB_ERROR_NO_MEM);

    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto second_fake = std::make_shared<FakeLibusb>();
    const auto second = LibusbRuntime::create(second_fake->functions());
    KB_CHECK(!second.has_value());
    KB_CHECK(second.error().kind == LibusbRuntimeErrorKind::already_running);
    runtime->stop();
    runtime->stop();
    KB_CHECK(fake->init_calls == 1);
    KB_CHECK(fake->exit_calls == 1);
}

void test_event_loop_and_filtered_utf8_enumeration() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    KB_CHECK(runtime->running());
    KB_CHECK(runtime->event_thread_id() != std::this_thread::get_id());

    UsbInterfaceFilter mismatch;
    mismatch.interface_protocol = 0x99;
    const auto empty = runtime->enumerate(mismatch);
    KB_CHECK(empty.has_value());
    KB_CHECK(empty->empty());
    KB_CHECK(fake->serial_calls == 0);

    const auto device = matching_device(runtime);
    KB_CHECK(device.vendor_id == 0x18D1);
    KB_CHECK(device.product_id == 0x4EE0);
    KB_CHECK(device.bus_number == 2);
    KB_CHECK(device.device_address == 5);
    KB_CHECK(device.backend_session_id == fake->session_data);
    KB_CHECK(device.configuration_value == 1);
    KB_CHECK(device.port_path == std::vector<std::uint8_t>({3, 4}));
    KB_CHECK(device.serial_utf8 == "serial-\xCE\xB1");
    KB_CHECK(device.interface_number == 2);
    KB_CHECK(device.bulk_out_endpoint == 0x01);
    KB_CHECK(device.bulk_out_max_packet_size == 4);
    KB_CHECK(device.bulk_in_endpoint == 0x81);
    KB_CHECK(device.bulk_in_max_packet_size == 4);
    KB_CHECK(fake->serial_calls == 1);
    runtime->stop();
}

void test_enumeration_retains_linux_topology_or_diagnostic() {
    auto fake = std::make_shared<FakeLibusb>();
    auto functions = fake->functions();
    std::optional<LinuxUsbTopologyQuery> observed_query;
    functions.resolve_linux_topology = [&](const LinuxUsbTopologyQuery& query) {
        observed_query = query;
        return std::expected<LinuxUsbTopology, LinuxUsbTopologyError>{
            LinuxUsbTopology{
                .physical_port_path = "usb:2-3.4",
                .root_controller_id = "linux-sysfs:pci0000:00/0000:00:14.0",
                .hub_port_chain = {3U, 4U},
                .vendor_id = query.vendor_id,
                .product_id = query.product_id,
                .bus_number = query.bus_number,
                .device_address = query.device_address,
                .serial_utf8 = query.serial_utf8,
                .product_utf8 = std::string{"USB Fastboot"},
                .sysfs_device_path =
                    "devices/pci0000:00/0000:00:14.0/usb2/2-3/2-3.4",
            }};
    };
    auto runtime = create_runtime(fake, std::move(functions));
    const auto enriched = matching_device(runtime);
    KB_CHECK(observed_query.has_value());
    KB_CHECK(observed_query->vendor_id == enriched.vendor_id);
    KB_CHECK(observed_query->product_id == enriched.product_id);
    KB_CHECK(observed_query->bus_number == enriched.bus_number);
    KB_CHECK(observed_query->device_address == enriched.device_address);
    KB_CHECK(observed_query->port_numbers == enriched.port_path);
    KB_CHECK(observed_query->serial_utf8 ==
             std::optional<std::string>{enriched.serial_utf8});
    KB_CHECK(enriched.linux_topology.has_value());
    KB_CHECK(enriched.linux_topology->physical_port_path == "usb:2-3.4");
    KB_CHECK(!enriched.linux_topology_error.has_value());
    runtime->stop();

    auto failing_fake = std::make_shared<FakeLibusb>();
    auto failing_functions = failing_fake->functions();
    failing_functions.resolve_linux_topology = [](const LinuxUsbTopologyQuery&) {
        return std::expected<LinuxUsbTopology, LinuxUsbTopologyError>{
            std::unexpected(LinuxUsbTopologyError{
                .kind = LinuxUsbTopologyErrorKind::PermissionDenied,
                .stage = LinuxUsbTopologyStage::AttributeRead,
                .native_code = EACCES,
                .path = "bus/usb/devices/2-3.4/idVendor",
                .message = "permission denied",
            })};
    };
    auto failing_runtime = create_runtime(failing_fake, std::move(failing_functions));
    const auto diagnosed = matching_device(failing_runtime);
    KB_CHECK(!diagnosed.linux_topology.has_value());
    KB_CHECK(diagnosed.linux_topology_error.has_value());
    KB_CHECK(diagnosed.linux_topology_error->kind ==
             LinuxUsbTopologyErrorKind::PermissionDenied);
    KB_CHECK(diagnosed.linux_topology_error->native_code == EACCES);
    failing_runtime->stop();
}

void test_enumeration_retains_macos_topology_or_diagnostic() {
    auto fake = std::make_shared<FakeLibusb>();
    auto functions = fake->functions();
    std::optional<MacUsbTopologyQuery> observed_query;
    functions.resolve_macos_topology = [
        &observed_query](const std::span<const MacUsbTopologyQuery> queries,
                         const auto,
                         const std::stop_token) {
        KB_CHECK(queries.size() == 1U);
        observed_query = queries.front();
        const auto& query = queries.front();
        return std::expected<std::vector<MacUsbTopology>, MacUsbTopologyError>{
            std::vector<MacUsbTopology>{MacUsbTopology{
                .physical_port_path = "usb:2-3.4",
                .root_controller_id = "macos-iokit:0000000000000011",
                .hub_port_chain = {3U, 4U},
                .registry_entry_id = 0x21U,
                .session_id = 0x31U,
                .interface_registry_entry_id = 0x41U,
                .location_id = 0x02340000U,
                .vendor_id = query.vendor_id,
                .product_id = query.product_id,
                .bus_number = query.bus_number,
                .device_address = query.device_address,
                .interface_fingerprint = query.interface_fingerprint,
                .serial_utf8 = query.serial_utf8,
                .product_utf8 = std::nullopt,
                .registry_path = "IOService:/USB/device",
                .interface_registry_path = "IOService:/USB/device/interface",
                .root_controller_registry_path = "IOService:/USB/controller",
            }}};
    };
    auto runtime = create_runtime(fake, std::move(functions));
    const auto enriched = matching_device(runtime);
    KB_CHECK(observed_query.has_value());
    KB_CHECK(observed_query->vendor_id == enriched.vendor_id);
    KB_CHECK(observed_query->product_id == enriched.product_id);
    KB_CHECK(observed_query->bus_number == enriched.bus_number);
    KB_CHECK(observed_query->device_address == enriched.device_address);
    KB_CHECK(observed_query->port_numbers == enriched.port_path);
    KB_CHECK(observed_query->interface_fingerprint.configuration_value ==
             enriched.configuration_value);
    KB_CHECK(observed_query->interface_fingerprint.interface_number ==
             enriched.interface_number);
    KB_CHECK(observed_query->interface_fingerprint.alternate_setting ==
             enriched.alternate_setting);
    KB_CHECK(observed_query->serial_utf8 ==
             std::optional<std::string>{enriched.serial_utf8});
    KB_CHECK(enriched.macos_topology.has_value());
    KB_CHECK(enriched.macos_topology->physical_port_path == "usb:2-3.4");
    KB_CHECK(!enriched.macos_topology_error.has_value());
    runtime->stop();

    auto failing_fake = std::make_shared<FakeLibusb>();
    auto failing_functions = failing_fake->functions();
    failing_functions.resolve_macos_topology = [](
        const std::span<const MacUsbTopologyQuery>,
        const auto,
        const std::stop_token) {
        return std::expected<std::vector<MacUsbTopology>, MacUsbTopologyError>{
            std::unexpected(MacUsbTopologyError{
                .kind = MacUsbTopologyErrorKind::PermissionDenied,
                .stage = MacUsbTopologyStage::DeviceSnapshot,
                .native_code = -1,
                .registry_path = "IOService:/USB/device",
                .message = "permission denied",
            })};
    };
    auto failing_runtime = create_runtime(failing_fake,
                                          std::move(failing_functions));
    const auto diagnosed = matching_device(failing_runtime);
    KB_CHECK(!diagnosed.macos_topology.has_value());
    KB_CHECK(diagnosed.macos_topology_error.has_value());
    KB_CHECK(diagnosed.macos_topology_error->kind ==
             MacUsbTopologyErrorKind::PermissionDenied);
    KB_CHECK(diagnosed.macos_topology_error->native_code == -1);
    failing_runtime->stop();
}

void test_device_topology_is_resolved_once_for_distinct_interfaces() {
    auto fake = std::make_shared<FakeLibusb>();
    auto functions = fake->functions();

    std::array<std::array<libusb_endpoint_descriptor, 2U>, 2U> endpoints{};
    std::array<libusb_interface_descriptor, 2U> alternates{};
    std::array<libusb_interface, 2U> interfaces{};
    for (std::size_t index = 0; index < alternates.size(); ++index) {
        endpoints[index][0].bEndpointAddress =
            static_cast<std::uint8_t>(0x01U + index);
        endpoints[index][0].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
        endpoints[index][0].wMaxPacketSize = 512U;
        endpoints[index][1].bEndpointAddress =
            static_cast<std::uint8_t>(0x81U + index);
        endpoints[index][1].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
        endpoints[index][1].wMaxPacketSize = 512U;

        alternates[index].bInterfaceNumber =
            static_cast<std::uint8_t>(2U + index);
        alternates[index].bAlternateSetting = 0U;
        alternates[index].bNumEndpoints = 2U;
        alternates[index].bInterfaceClass = 0xFFU;
        alternates[index].bInterfaceSubClass = 0x42U;
        alternates[index].bInterfaceProtocol = 0x03U;
        alternates[index].endpoint = endpoints[index].data();
        interfaces[index].altsetting = &alternates[index];
        interfaces[index].num_altsetting = 1;
    }
    libusb_config_descriptor config{};
    config.bConfigurationValue = 1U;
    config.bNumInterfaces = static_cast<std::uint8_t>(interfaces.size());
    config.interface = interfaces.data();
    functions.get_active_config_descriptor =
        [&config](libusb_device*, libusb_config_descriptor** output) {
            *output = &config;
            return LIBUSB_SUCCESS;
        };
    functions.get_config_descriptor =
        [&config](libusb_device*,
                  std::uint8_t,
                  libusb_config_descriptor** output) {
            *output = &config;
            return LIBUSB_SUCCESS;
        };

    std::size_t resolver_calls = 0U;
    functions.resolve_linux_topology =
        [&resolver_calls](const LinuxUsbTopologyQuery& query) {
            ++resolver_calls;
            return std::expected<LinuxUsbTopology, LinuxUsbTopologyError>{
                LinuxUsbTopology{
                    .physical_port_path = "usb:2-3.4",
                    .root_controller_id =
                        "linux-sysfs:pci0000:00/0000:00:14.0",
                    .hub_port_chain = {3U, 4U},
                    .vendor_id = query.vendor_id,
                    .product_id = query.product_id,
                    .bus_number = query.bus_number,
                    .device_address = query.device_address,
                    .serial_utf8 = query.serial_utf8,
                    .product_utf8 = std::nullopt,
                    .sysfs_device_path =
                        "devices/pci0000:00/0000:00:14.0/usb2/2-3/2-3.4",
                }};
        };
    std::vector<WindowsUsbTopologyQuery> windows_queries;
    std::size_t windows_identity_captures = 0U;
    const std::string windows_instance_id =
        "USB\\VID_18D1&PID_4EE0\\SERIAL";
    functions.capture_windows_session_identity =
        [&windows_identity_captures,
         &windows_instance_id](const unsigned long session) {
            ++windows_identity_captures;
            KB_CHECK(session == 0x101UL);
            return std::expected<std::string, WindowsUsbTopologyError>{
                windows_instance_id};
        };
    functions.resolve_windows_topology =
        [&windows_queries](const WindowsUsbTopologyQuery& query) {
            windows_queries.push_back(query);
            return std::expected<WindowsUsbTopology,
                                 WindowsUsbTopologyError>{
                WindowsUsbTopology{
                    .physical_port_path = "usb:2-3.4",
                    .root_controller_id =
                        "windows-pnp:PCI\\VEN_8086&DEV_7AE0\\CONTROLLER-01",
                    .hub_port_chain = query.port_numbers,
                    .vendor_id = query.vendor_id,
                    .product_id = query.product_id,
                    .bus_number = query.bus_number,
                    .device_address = query.device_address,
                    .serial_utf8 = query.serial_utf8,
                    .interface_fingerprint = query.interface_fingerprint,
                    .device_instance_id_utf8 =
                        "USB\\VID_18D1&PID_4EE0\\SERIAL",
                    .hub_instance_ids_utf8 = {
                        "USB\\ROOT_HUB30\\ROOT-01",
                        "USB\\VID_2109&PID_2817\\EXTERNAL-HUB",
                    },
                    .location_path_utf8 =
                        "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(3)#USB(4)",
                }};
        };

    std::size_t macos_resolver_calls = 0U;
    std::vector<MacUsbTopologyQuery> macos_queries;
    functions.resolve_macos_topology = [
        &macos_resolver_calls,
        &macos_queries](const std::span<const MacUsbTopologyQuery> queries,
                        const auto deadline,
                        const std::stop_token cancellation) {
        ++macos_resolver_calls;
        const auto now = std::chrono::steady_clock::now();
        KB_CHECK(deadline > now);
        KB_CHECK(deadline <= now + std::chrono::seconds{5});
        KB_CHECK(!cancellation.stop_requested());
        macos_queries.assign(queries.begin(), queries.end());
        std::vector<MacUsbTopology> results;
        results.reserve(queries.size());
        for (std::size_t index = 0U; index < queries.size(); ++index) {
            const auto& query = queries[index];
            results.push_back(MacUsbTopology{
                .physical_port_path = "usb:2-3.4",
                .root_controller_id = "macos-iokit:0000000000000011",
                .hub_port_chain = query.port_numbers,
                .registry_entry_id = 0x21U,
                .session_id = query.session_id,
                .interface_registry_entry_id = 0x41U + index,
                .location_id = 0x02340000U,
                .vendor_id = query.vendor_id,
                .product_id = query.product_id,
                .bus_number = query.bus_number,
                .device_address = query.device_address,
                .interface_fingerprint = query.interface_fingerprint,
                .serial_utf8 = query.serial_utf8,
                .product_utf8 = std::nullopt,
                .registry_path = "IOService:/USB/device",
                .interface_registry_path =
                    "IOService:/USB/device/interface-" +
                    std::to_string(index),
                .root_controller_registry_path = "IOService:/USB/controller",
            });
        }
        return std::expected<std::vector<MacUsbTopology>, MacUsbTopologyError>{
            std::move(results)};
    };

    auto runtime = create_runtime(fake, std::move(functions));
    UsbInterfaceFilter filter;
    filter.interface_class = 0xFFU;
    filter.interface_subclass = 0x42U;
    filter.interface_protocol = 0x03U;
    const auto enumerated = runtime->enumerate(filter);
    KB_CHECK(enumerated.has_value());
    KB_CHECK(enumerated->size() == 2U);
    KB_CHECK(resolver_calls == 1U);
    KB_CHECK(macos_resolver_calls == 1U);
    KB_CHECK(macos_queries.size() == 2U);
    KB_CHECK(macos_queries[0].session_id != 0U);
    KB_CHECK(macos_queries[0].session_id == macos_queries[1].session_id);
    KB_CHECK(macos_queries[0].interface_fingerprint !=
             macos_queries[1].interface_fingerprint);
    KB_CHECK((*enumerated)[0].interface_number !=
             (*enumerated)[1].interface_number);
    KB_CHECK((*enumerated)[0].linux_topology.has_value());
    KB_CHECK((*enumerated)[1].linux_topology.has_value());
    KB_CHECK((*enumerated)[0].linux_topology ==
             (*enumerated)[1].linux_topology);
    KB_CHECK(!(*enumerated)[0].linux_topology_error.has_value());
    KB_CHECK(!(*enumerated)[1].linux_topology_error.has_value());
    KB_CHECK(fake->session_data_calls == 2);
    KB_CHECK(windows_identity_captures == 2U);
    KB_CHECK(windows_queries.size() == 2U);
    for (std::size_t index = 0U; index < windows_queries.size(); ++index) {
        const auto& query = windows_queries[index];
        const auto& snapshot = (*enumerated)[index];
        KB_CHECK(query.libusb_session_data == fake->session_data);
        KB_CHECK(query.device_instance_id_utf8 == windows_instance_id);
        KB_CHECK(query.vendor_id == snapshot.vendor_id);
        KB_CHECK(query.product_id == snapshot.product_id);
        KB_CHECK(query.bus_number == snapshot.bus_number);
        KB_CHECK(query.device_address == snapshot.device_address);
        KB_CHECK(query.port_numbers == snapshot.port_path);
        KB_CHECK(query.serial_utf8 ==
                 std::optional<std::string>{snapshot.serial_utf8});
        KB_CHECK(query.interface_fingerprint.interface_number ==
                 snapshot.interface_number);
        KB_CHECK(query.interface_fingerprint.interface_class ==
                 snapshot.interface_class);
        KB_CHECK(query.interface_fingerprint.interface_subclass ==
                 snapshot.interface_subclass);
        KB_CHECK(query.interface_fingerprint.interface_protocol ==
                 snapshot.interface_protocol);
        KB_CHECK(snapshot.windows_topology.has_value());
        KB_CHECK(snapshot.windows_topology->interface_fingerprint ==
                 query.interface_fingerprint);
        KB_CHECK(!snapshot.windows_topology_error.has_value());
    }
    KB_CHECK((*enumerated)[0].macos_topology.has_value());
    KB_CHECK((*enumerated)[1].macos_topology.has_value());
    KB_CHECK((*enumerated)[0].macos_topology->session_id ==
             (*enumerated)[1].macos_topology->session_id);
    KB_CHECK((*enumerated)[0].macos_topology->interface_registry_entry_id !=
             (*enumerated)[1].macos_topology->interface_registry_entry_id);
    runtime->stop();
}

void test_zero_windows_session_is_diagnostic_and_never_resolved() {
    auto fake = std::make_shared<FakeLibusb>();
    fake->session_data = 0UL;
    auto functions = fake->functions();
    std::size_t resolver_calls = 0U;
    std::size_t identity_capture_calls = 0U;
    functions.capture_windows_session_identity =
        [&identity_capture_calls](const unsigned long) {
            ++identity_capture_calls;
            return std::expected<std::string, WindowsUsbTopologyError>{
                "USB\\VID_18D1&PID_4EE0\\SHOULD-NOT-BE-CAPTURED"};
        };
    functions.resolve_windows_topology =
        [&resolver_calls](const WindowsUsbTopologyQuery&) {
            ++resolver_calls;
            return std::expected<WindowsUsbTopology,
                                 WindowsUsbTopologyError>{
                std::unexpected(WindowsUsbTopologyError{})};
        };

    auto runtime = create_runtime(fake, std::move(functions));
    const auto snapshot = matching_device(runtime);
    KB_CHECK(fake->session_data_calls == 1);
    KB_CHECK(identity_capture_calls == 0U);
    KB_CHECK(resolver_calls == 0U);
    KB_CHECK(!snapshot.windows_topology.has_value());
    KB_CHECK(snapshot.windows_topology_error.has_value());
    KB_CHECK(snapshot.windows_topology_error->kind ==
             WindowsUsbTopologyErrorKind::InvalidArgument);
    KB_CHECK(snapshot.windows_topology_error->stage ==
             WindowsUsbTopologyStage::Validation);
    KB_CHECK(snapshot.windows_topology_error->native_domain ==
             WindowsUsbNativeErrorDomain::None);
    KB_CHECK(snapshot.windows_topology_error->native_code == 0U);
    KB_CHECK(snapshot.windows_topology_error->libusb_session_data == 0UL);
    runtime->stop();
}

void test_windows_session_identity_capture_failure_is_diagnostic() {
    auto fake = std::make_shared<FakeLibusb>();
    auto functions = fake->functions();
    const WindowsUsbTopologyError identity_error{
        .kind = WindowsUsbTopologyErrorKind::IdentityChanged,
        .stage = WindowsUsbTopologyStage::Enumeration,
        .native_domain =
            WindowsUsbNativeErrorDomain::ConfigurationManager,
        .native_code = 0x0DU,
        .libusb_session_data = fake->session_data,
        .device_instance_id_utf8 =
            "USB\\VID_18D1&PID_4EE0\\REMOVED",
        .message = "the libusb DEVINST disappeared during identity capture",
    };
    std::size_t identity_capture_calls = 0U;
    std::size_t resolver_calls = 0U;
    functions.capture_windows_session_identity =
        [&identity_capture_calls,
         &identity_error](const unsigned long session) {
            ++identity_capture_calls;
            KB_CHECK(session == identity_error.libusb_session_data);
            return std::expected<std::string, WindowsUsbTopologyError>{
                std::unexpected(identity_error)};
        };
    functions.resolve_windows_topology =
        [&resolver_calls](const WindowsUsbTopologyQuery&) {
            ++resolver_calls;
            return std::expected<WindowsUsbTopology,
                                 WindowsUsbTopologyError>{
                WindowsUsbTopology{}};
        };

    auto runtime = create_runtime(fake, std::move(functions));
    const auto snapshot = matching_device(runtime);
    KB_CHECK(fake->session_data_calls == 1);
    KB_CHECK(identity_capture_calls == 1U);
    KB_CHECK(resolver_calls == 0U);
    KB_CHECK(!snapshot.windows_topology.has_value());
    KB_CHECK(snapshot.windows_topology_error ==
             std::optional<WindowsUsbTopologyError>{identity_error});
    runtime->stop();
}

void test_windows_devinst_reuse_never_publishes_stale_libusb_identity() {
    auto fake = std::make_shared<FakeLibusb>();
    auto functions = fake->functions();
    const std::array<std::string_view, 2> generations{
        "USB\\VID_18D1&PID_4EE0\\GENERATION-A",
        "USB\\VID_18D1&PID_4EE0\\GENERATION-B",
    };
    std::size_t identity_captures = 0U;
    std::size_t resolver_calls = 0U;
    functions.capture_windows_session_identity =
        [&identity_captures,
         &generations](const unsigned long session) {
            KB_CHECK(session == 0x101UL);
            const auto index = std::min(identity_captures,
                                        generations.size() - 1U);
            ++identity_captures;
            return std::expected<std::string, WindowsUsbTopologyError>{
                std::string{generations[index]}};
        };
    functions.resolve_windows_topology =
        [&resolver_calls](const WindowsUsbTopologyQuery&) {
            ++resolver_calls;
            return std::expected<WindowsUsbTopology,
                                 WindowsUsbTopologyError>{
                WindowsUsbTopology{}};
        };

    auto runtime = create_runtime(fake, std::move(functions));
    const auto snapshot = matching_device(runtime);
    KB_CHECK(fake->session_data_calls == 2);
    KB_CHECK(identity_captures == 2U);
    KB_CHECK(resolver_calls == 0U);
    KB_CHECK(!snapshot.windows_topology.has_value());
    KB_CHECK(snapshot.windows_topology_error.has_value());
    KB_CHECK(snapshot.windows_topology_error->kind ==
             WindowsUsbTopologyErrorKind::IdentityChanged);
    KB_CHECK(snapshot.windows_topology_error->stage ==
             WindowsUsbTopologyStage::StabilityCheck);
    KB_CHECK(snapshot.windows_topology_error->device_instance_id_utf8 ==
             generations[1]);
    runtime->stop();
}

void test_windows_generation_revalidation_rejects_stale_address_and_serial() {
    const auto run_case = [](const bool change_address,
                             const bool change_serial) {
        auto fake = std::make_shared<FakeLibusb>();
        auto functions = fake->functions();
        std::size_t address_reads = 0U;
        std::size_t serial_reads = 0U;
        std::size_t identity_captures = 0U;
        std::size_t resolver_calls = 0U;
        const std::string instance_id =
            "USB\\VID_18D1&PID_4EE0\\STABLE-GENERATION";
        functions.get_device_address =
            [&address_reads, change_address](libusb_device*) {
                ++address_reads;
                return static_cast<std::uint8_t>(
                    change_address && address_reads > 1U ? 6U : 5U);
            };
        functions.get_device_string =
            [&serial_reads,
             change_serial](libusb_device*,
                            libusb_device_string_type,
                            char* output,
                            const int length) -> int {
                ++serial_reads;
                const std::string_view value =
                    change_serial && serial_reads > 1U
                    ? std::string_view{"SERIAL-REPLACED"}
                    : std::string_view{"FASTBOOT-SERIAL"};
                KB_CHECK(length > static_cast<int>(value.size()));
                std::memcpy(output, value.data(), value.size());
                output[value.size()] = '\0';
                return static_cast<int>(value.size() + 1U);
            };
        functions.capture_windows_session_identity =
            [&identity_captures,
             &instance_id](const unsigned long session) {
                ++identity_captures;
                KB_CHECK(session == 0x101UL);
                return std::expected<std::string,
                                     WindowsUsbTopologyError>{instance_id};
            };
        functions.resolve_windows_topology =
            [&resolver_calls](const WindowsUsbTopologyQuery&) {
                ++resolver_calls;
                return std::expected<WindowsUsbTopology,
                                     WindowsUsbTopologyError>{
                    WindowsUsbTopology{}};
            };

        auto runtime = create_runtime(fake, std::move(functions));
        const auto snapshot = matching_device(runtime);
        KB_CHECK(address_reads == 2U);
        KB_CHECK(serial_reads == 2U);
        KB_CHECK(identity_captures == 2U);
        KB_CHECK(resolver_calls == 0U);
        KB_CHECK(!snapshot.windows_topology.has_value());
        KB_CHECK(snapshot.windows_topology_error.has_value());
        KB_CHECK(snapshot.windows_topology_error->kind ==
                 WindowsUsbTopologyErrorKind::IdentityChanged);
        KB_CHECK(snapshot.windows_topology_error->stage ==
                 WindowsUsbTopologyStage::StabilityCheck);
        runtime->stop();
    };

    run_case(true, false);
    run_case(false, true);
}

void test_runtime_stop_cancels_macos_topology_outside_lifecycle_lock() {
    auto fake = std::make_shared<FakeLibusb>();
    auto functions = fake->functions();
    std::promise<void> resolver_entered;
    auto entered = resolver_entered.get_future();
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    const auto expected_session = fake->session_data;
    functions.resolve_macos_topology = [
        &resolver_entered,
        &wait_mutex,
        &wait_cv,
        expected_session](const std::span<const MacUsbTopologyQuery> queries,
                  const auto deadline,
                  const std::stop_token cancellation) {
        KB_CHECK(queries.size() == 1U);
        KB_CHECK(queries.front().session_id == expected_session);
        KB_CHECK(deadline != std::chrono::steady_clock::time_point::max());
        resolver_entered.set_value();
        std::stop_callback wake(cancellation, [&wait_cv] { wait_cv.notify_all(); });
        std::unique_lock lock(wait_mutex);
        wait_cv.wait_until(lock, deadline, [&cancellation] {
            return cancellation.stop_requested();
        });
        KB_CHECK(cancellation.stop_requested());
        return std::expected<std::vector<MacUsbTopology>, MacUsbTopologyError>{
            std::unexpected(MacUsbTopologyError{
                .kind = MacUsbTopologyErrorKind::Cancelled,
                .stage = MacUsbTopologyStage::DeviceEnumeration,
                .message = "cancelled by runtime stop",
            })};
    };

    auto runtime = create_runtime(fake, std::move(functions));
    auto enumeration = std::async(std::launch::async, [runtime] {
        UsbInterfaceFilter filter;
        filter.interface_class = 0xFFU;
        filter.interface_subclass = 0x42U;
        filter.interface_protocol = 0x03U;
        return runtime->enumerate(filter);
    });
    KB_CHECK(entered.wait_for(std::chrono::seconds{1}) ==
             std::future_status::ready);
    const auto stop_started = std::chrono::steady_clock::now();
    runtime->stop();
    KB_CHECK(std::chrono::steady_clock::now() - stop_started <
             std::chrono::seconds{1});
    KB_CHECK(enumeration.wait_for(std::chrono::seconds{1}) ==
             std::future_status::ready);
    const auto result = enumeration.get();
    KB_CHECK(!result.has_value());
    KB_CHECK(result.error().kind == LibusbRuntimeErrorKind::runtime_stopped);
}

void test_open_revalidates_physical_identity_and_address_reuse() {
    auto fake = std::make_shared<FakeLibusb>();
    auto table = fake->functions();
    auto base_open = table.open;
    std::array<std::byte, 3> device_storage{};
    std::array<libusb_device*, 2> enumeration_list{
        reinterpret_cast<libusb_device*>(&device_storage[0]), nullptr};
    std::array<libusb_device*, 4> reopen_list{
        reinterpret_cast<libusb_device*>(&device_storage[0]),
        reinterpret_cast<libusb_device*>(&device_storage[1]),
        reinterpret_cast<libusb_device*>(&device_storage[2]),
        nullptr};
    std::array<libusb_device*, 2> empty_serial_list{
        reinterpret_cast<libusb_device*>(&device_storage[1]), nullptr};
    bool reopening = false;
    bool testing_empty_serial_rule = false;
    std::size_t opened_index = std::numeric_limits<std::size_t>::max();

    const auto index_of = [&](libusb_device* candidate) {
        for (std::size_t index = 0; index < device_storage.size(); ++index) {
            if (candidate == reinterpret_cast<libusb_device*>(&device_storage[index])) {
                return index;
            }
        }
        throw TestFailure("unknown fake device");
    };
    table.get_device_list = [&](libusb_context*, libusb_device*** list) -> ssize_t {
        if (testing_empty_serial_rule) {
            *list = empty_serial_list.data();
            return 1;
        }
        if (reopening) {
            *list = reopen_list.data();
            return 3;
        }
        *list = enumeration_list.data();
        return 1;
    };
    table.get_bus_number = [](libusb_device*) { return std::uint8_t{2}; };
    table.get_device_address = [&](libusb_device* candidate) {
        constexpr std::array<std::uint8_t, 3> addresses{5, 5, 8};
        return addresses[index_of(candidate)];
    };
    table.get_port_numbers = [&](libusb_device* candidate,
                                 std::uint8_t* path,
                                 int length) -> int {
        KB_CHECK(length >= 2);
        const auto index = index_of(candidate);
        if (reopening && index == 0) {
            path[0] = 9;
            path[1] = 9;
        } else {
            path[0] = 3;
            path[1] = 4;
        }
        return 2;
    };
    table.get_device_string = [&](libusb_device* candidate,
                                  libusb_device_string_type,
                                  char* output,
                                  int length) -> int {
        const auto index = index_of(candidate);
        const std::string_view serial = reopening && index == 1
                                            ? std::string_view{"wrong-serial"}
                                            : std::string_view{"serial-\xCE\xB1"};
        KB_CHECK(length > static_cast<int>(serial.size()));
        std::memcpy(output, serial.data(), serial.size());
        output[serial.size()] = '\0';
        return static_cast<int>(serial.size() + 1U);
    };
    table.open = [&](libusb_device* candidate, libusb_device_handle** handle) {
        opened_index = index_of(candidate);
        return base_open(candidate, handle);
    };

    auto runtime = create_runtime(fake, std::move(table));
    const auto snapshot = matching_device(runtime);
    KB_CHECK(snapshot.device_address == 5);
    reopening = true;

    auto backend_result = runtime->open_bulk_out(snapshot);
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);
    KB_CHECK(opened_index == 2);
    KB_CHECK(fake->open_calls == 1);

    backend->stop();
    auto serial_unavailable_snapshot = snapshot;
    serial_unavailable_snapshot.serial_utf8.clear();
    testing_empty_serial_rule = true;
    opened_index = std::numeric_limits<std::size_t>::max();
    auto empty_serial_backend_result =
        runtime->open_bulk_out(serial_unavailable_snapshot);
    KB_CHECK(empty_serial_backend_result.has_value());
    auto empty_serial_backend = std::move(*empty_serial_backend_result);
    KB_CHECK(opened_index == 1);
    empty_serial_backend->stop();

    auto invalid_snapshot = snapshot;
    invalid_snapshot.port_path.clear();
    const auto invalid = runtime->open_bulk_out(invalid_snapshot);
    KB_CHECK(!invalid.has_value());
    KB_CHECK(invalid.error().kind == LibusbRuntimeErrorKind::invalid_device);

    runtime->stop();
}

void test_open_rejects_changed_interface_snapshot() {
    auto fake = std::make_shared<FakeLibusb>();
    auto table = fake->functions();
    auto base_active_config = table.get_active_config_descriptor;
    auto base_config = table.get_config_descriptor;
    bool interface_changed = false;

    libusb_endpoint_descriptor changed_endpoints[2]{};
    changed_endpoints[0].bEndpointAddress = 0x01;
    changed_endpoints[0].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
    changed_endpoints[0].wMaxPacketSize = 4;
    changed_endpoints[1].bEndpointAddress = 0x82;
    changed_endpoints[1].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
    changed_endpoints[1].wMaxPacketSize = 4;
    libusb_interface_descriptor changed_alternate{};
    changed_alternate.bInterfaceNumber = 2;
    changed_alternate.bAlternateSetting = 0;
    changed_alternate.bNumEndpoints = 2;
    changed_alternate.bInterfaceClass = 0xFF;
    changed_alternate.bInterfaceSubClass = 0x42;
    changed_alternate.bInterfaceProtocol = 0x03;
    changed_alternate.endpoint = changed_endpoints;
    libusb_interface changed_interface{};
    changed_interface.altsetting = &changed_alternate;
    changed_interface.num_altsetting = 1;
    libusb_config_descriptor changed_config{};
    changed_config.bConfigurationValue = 1;
    changed_config.bNumInterfaces = 1;
    changed_config.interface = &changed_interface;

    table.get_active_config_descriptor =
        [&](libusb_device* candidate, libusb_config_descriptor** config) -> int {
            if (interface_changed) {
                *config = &changed_config;
                return LIBUSB_SUCCESS;
            }
            return base_active_config(candidate, config);
        };
    table.get_config_descriptor =
        [&](libusb_device* candidate,
            std::uint8_t index,
            libusb_config_descriptor** config) -> int {
            if (interface_changed) {
                *config = &changed_config;
                return LIBUSB_SUCCESS;
            }
            return base_config(candidate, index, config);
        };

    auto runtime = create_runtime(fake, std::move(table));
    const auto snapshot = matching_device(runtime);
    interface_changed = true;
    const auto backend = runtime->open_bulk_out(snapshot);
    KB_CHECK(!backend.has_value());
    KB_CHECK(backend.error().kind == LibusbRuntimeErrorKind::device_not_found);
    KB_CHECK(fake->open_calls == 0);
    runtime->stop();
}

void test_open_configuration_is_verified_before_claim() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);

    auto already_configured = runtime->open_bulk_out(snapshot);
    KB_CHECK(already_configured.has_value());
    KB_CHECK(fake->get_configuration_calls == 1);
    KB_CHECK(fake->set_configuration_calls == 0);
    KB_CHECK(fake->claim_calls == 1);
    KB_CHECK(fake->get_configuration_order < fake->claim_order);
    (*already_configured)->stop();

    fake->current_configuration = 0;
    fake->active_config_result = LIBUSB_ERROR_NOT_FOUND;
    fake->open_sequence = 0;
    fake->get_configuration_order = 0;
    fake->set_configuration_order = 0;
    fake->claim_order = 0;
    auto configured_on_open = runtime->open_bulk_out(snapshot);
    KB_CHECK(configured_on_open.has_value());
    KB_CHECK(fake->get_configuration_calls == 2);
    KB_CHECK(fake->set_configuration_calls == 1);
    KB_CHECK(fake->last_set_configuration == snapshot.configuration_value);
    KB_CHECK(fake->current_configuration == snapshot.configuration_value);
    KB_CHECK(fake->get_configuration_order < fake->set_configuration_order);
    KB_CHECK(fake->set_configuration_order < fake->claim_order);
    (*configured_on_open)->stop();
    runtime->stop();
}

void test_open_configuration_failures_release_reservation() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);

    fake->get_configuration_result = LIBUSB_ERROR_IO;
    const auto query_failure = runtime->open_bulk_out(snapshot);
    KB_CHECK(!query_failure.has_value());
    KB_CHECK(query_failure.error().kind ==
             LibusbRuntimeErrorKind::configuration_failed);
    KB_CHECK(query_failure.error().native_code == LIBUSB_ERROR_IO);
    KB_CHECK(fake->claim_calls == 0);
    KB_CHECK(fake->close_calls == 1);

    fake->get_configuration_result = LIBUSB_SUCCESS;
    fake->current_configuration = 0;
    fake->set_configuration_result = LIBUSB_ERROR_ACCESS;
    const auto set_failure = runtime->open_bulk_out(snapshot);
    KB_CHECK(!set_failure.has_value());
    KB_CHECK(set_failure.error().kind ==
             LibusbRuntimeErrorKind::configuration_failed);
    KB_CHECK(set_failure.error().native_code == LIBUSB_ERROR_ACCESS);
    KB_CHECK(fake->claim_calls == 0);
    KB_CHECK(fake->close_calls == 2);

    fake->set_configuration_result = LIBUSB_SUCCESS;
    auto reopened = runtime->open_bulk_out(snapshot);
    KB_CHECK(reopened.has_value());
    KB_CHECK(fake->claim_calls == 1);
    (*reopened)->stop();
    runtime->stop();
}

void test_runtime_reservation_and_claim_busy_contract() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);

    auto first = runtime->open_bulk_out(snapshot);
    KB_CHECK(first.has_value());
    const auto open_calls_before_duplicate = fake->open_calls.load();
    const auto claim_calls_before_duplicate = fake->claim_calls.load();
    const auto duplicate = runtime->open_bulk_out(snapshot);
    KB_CHECK(!duplicate.has_value());
    KB_CHECK(duplicate.error().kind == LibusbRuntimeErrorKind::interface_busy);
    KB_CHECK(duplicate.error().native_code == LIBUSB_ERROR_BUSY);
    KB_CHECK(fake->open_calls == open_calls_before_duplicate);
    KB_CHECK(fake->claim_calls == claim_calls_before_duplicate);

    (*first)->stop();
    auto reopened = runtime->open_bulk_out(snapshot);
    KB_CHECK(reopened.has_value());
    (*reopened)->stop();

    fake->claim_result = LIBUSB_ERROR_BUSY;
    const auto claim_busy = runtime->open_bulk_out(snapshot);
    KB_CHECK(!claim_busy.has_value());
    KB_CHECK(claim_busy.error().kind == LibusbRuntimeErrorKind::interface_busy);
    KB_CHECK(claim_busy.error().native_code == LIBUSB_ERROR_BUSY);

    fake->claim_result = LIBUSB_SUCCESS;
    auto after_claim_busy = runtime->open_bulk_out(snapshot);
    KB_CHECK(after_claim_busy.has_value());
    (*after_claim_busy)->stop();
    runtime->stop();
}

void test_out_of_order_completion_and_payload_lifetime() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto backend_result = runtime->open_bulk_out(matching_device(runtime));
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);

    auto first_payload = payload(4);
    auto second_payload = payload(4);
    std::weak_ptr<std::vector<std::byte>> first_lifetime = first_payload;
    std::weak_ptr<std::vector<std::byte>> second_lifetime = second_payload;
    KB_CHECK(backend->submit(TransferSubmission{1, 0, *first_payload, first_payload}) ==
             SubmitResult::accepted);
    KB_CHECK(backend->submit(TransferSubmission{2, 4, *second_payload, second_payload}) ==
             SubmitResult::accepted);
    first_payload.reset();
    second_payload.reset();
    KB_CHECK(!first_lifetime.expired());
    KB_CHECK(!second_lifetime.expired());

    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto second = wait_for_completion(*backend);
    const auto first = wait_for_completion(*backend);
    KB_CHECK(second.id == 2);
    KB_CHECK(first.id == 1);
    KB_CHECK(second.code == CompletionCode::success);
    KB_CHECK(first.code == CompletionCode::success);
    KB_CHECK(fake->callback_thread == runtime->event_thread_id());
    KB_CHECK(fake->callback_thread != std::this_thread::get_id());
    KB_CHECK(first_lifetime.expired());
    KB_CHECK(second_lifetime.expired());
    KB_CHECK(backend->in_flight() == 0);

    {
        std::vector<std::byte> transient{std::byte{0x11},
                                         std::byte{0x22},
                                         std::byte{0x33}};
        KB_CHECK(backend->submit(TransferSubmission{3, 8, transient, {}}) ==
                 SubmitResult::accepted);
    }
    const auto copied_submission = fake->submission(2);
    KB_CHECK(copied_submission.length == 3);
    KB_CHECK(copied_submission.buffer[0] == 0x11);
    KB_CHECK(copied_submission.buffer[2] == 0x33);
    fake->complete_submission(2, LIBUSB_TRANSFER_COMPLETED, 3);
    const auto copied_completion = wait_for_completion(*backend);
    KB_CHECK(copied_completion.id == 3);
    KB_CHECK(copied_completion.code == CompletionCode::success);

    backend->stop();
    runtime->stop();
}

void test_submit_and_completion_error_classification() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto backend_result = runtime->open_bulk_out(matching_device(runtime));
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);
    auto bytes = payload(4);

    fake->queue_submit_result(LIBUSB_ERROR_NO_DEVICE);
    KB_CHECK(backend->submit(TransferSubmission{1, 0, *bytes, bytes}) ==
             SubmitResult::no_device);
    KB_CHECK(backend->in_flight() == 0);
    KB_CHECK(fake->free_transfer_calls == 1);

    fake->queue_submit_result(LIBUSB_ERROR_NO_MEM);
    KB_CHECK(backend->submit(TransferSubmission{2, 0, *bytes, bytes}) ==
             SubmitResult::resource_exhausted);

    KB_CHECK(backend->submit(TransferSubmission{3, 0, *bytes, bytes}) ==
             SubmitResult::accepted);
    fake->complete_submission(0, LIBUSB_TRANSFER_NO_DEVICE, 2);
    const auto completion = wait_for_completion(*backend);
    KB_CHECK(completion.id == 3);
    KB_CHECK(completion.code == CompletionCode::no_device);
    KB_CHECK(completion.transferred_bytes == 2);

    backend->stop();
    runtime->stop();
}

void test_submit_allocation_failures_do_not_throw_or_leak() {
    auto fake = std::make_shared<FakeLibusb>();
    auto table = fake->functions();
    std::optional<LibusbSubmitFaultPoint> injected_fault;
    table.submit_allocation_fault = [&](const LibusbSubmitFaultPoint point) {
        if (injected_fault == point) {
            throw std::bad_alloc{};
        }
    };
    auto runtime = create_runtime(fake, std::move(table));
    auto backend_result = runtime->open_bulk_out(matching_device(runtime));
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);
    const std::vector<std::byte> transient(4, std::byte{0x5A});

    constexpr std::array fault_points{
        LibusbSubmitFaultPoint::pending_allocation,
        LibusbSubmitFaultPoint::fallback_payload_allocation,
        LibusbSubmitFaultPoint::registry_allocation,
    };
    TransferId id = 100;
    for (const auto point : fault_points) {
        injected_fault = point;
        const auto freed_before = fake->free_transfer_calls.load();
        KB_CHECK(backend->submit(TransferSubmission{id++, 0, transient, {}}) ==
                 SubmitResult::resource_exhausted);
        KB_CHECK(fake->free_transfer_calls == freed_before + 1);
        KB_CHECK(backend->in_flight() == 0);
        KB_CHECK(fake->submission_count() == 0);
    }

    injected_fault.reset();
    fake->fail_allocation = true;
    const auto freed_before_null = fake->free_transfer_calls.load();
    KB_CHECK(backend->submit(TransferSubmission{id++, 0, transient, {}}) ==
             SubmitResult::resource_exhausted);
    KB_CHECK(fake->free_transfer_calls == freed_before_null);
    fake->fail_allocation = false;

    KB_CHECK(backend->submit(TransferSubmission{id, 0, transient, {}}) ==
             SubmitResult::accepted);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    KB_CHECK(wait_for_completion(*backend).id == id);
    backend->stop();
    runtime->stop();
}

void test_transfer_ring_adapter_lifetime_and_completion_flow() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const BulkOutOptions explicit_message_end_zlp{
        0, ZeroPacketPolicy::when_logical_message_end_packet_aligned};
    auto backend_result =
        runtime->open_bulk_out(matching_device(runtime), explicit_message_end_zlp);
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(*backend, budget, TransferRingConfig{4, 2});
    std::vector<std::byte> source_bytes(8, std::byte{0x5A});

    KB_CHECK(ring.start(
        std::make_shared<MemoryTransferSource>(std::move(source_bytes))));
    KB_CHECK(fake->submission_count() == 2);
    KB_CHECK(budget->used() == 8);
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);

    const auto second = wait_for_completion(*backend);
    KB_CHECK(second.id == 2);
    KB_CHECK(ring.handle_completion(second));
    KB_CHECK(budget->used() == 4);
    const auto first = wait_for_completion(*backend);
    KB_CHECK(first.id == 1);
    KB_CHECK(ring.handle_completion(first));
    KB_CHECK(ring.state() == TransferRingState::completed);
    KB_CHECK(ring.completion_watermark() == 8);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(fake->submission_count() == 2);

    backend->stop();
    runtime->stop();
}

void test_observer_cancel_certainty_includes_submitted_and_out_of_order_data() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto backend_result = runtime->open_bulk_out(matching_device(runtime));
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);
    const auto budget = std::make_shared<BufferBudget>(8);
    TransferRing ring(*backend, budget, TransferRingConfig{4, 2});
    std::vector<std::byte> source_bytes(8, std::byte{0x5A});

    KB_CHECK(ring.start(
        std::make_shared<MemoryTransferSource>(std::move(source_bytes))));
    KB_CHECK(ring.completion_watermark() == 0);
    KB_CHECK(ring.submitted_bytes() == 8);
    KB_CHECK(ring.in_flight() == 2);
    KB_CHECK(kairosboot::transport::detail::observer_cancel_certainty(ring) ==
             TransferCertainty::PartialOrUnknown);

    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto out_of_order = wait_for_completion(*backend);
    KB_CHECK(out_of_order.id == 2);
    KB_CHECK(ring.handle_completion(out_of_order));
    KB_CHECK(ring.completion_watermark() == 0);
    KB_CHECK(ring.completed_bytes() == 4);
    KB_CHECK(ring.in_flight() == 1);
    KB_CHECK(kairosboot::transport::detail::observer_cancel_certainty(ring) ==
             TransferCertainty::PartialOrUnknown);

    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    KB_CHECK(ring.handle_completion(wait_for_completion(*backend)));
    KB_CHECK(kairosboot::transport::detail::observer_cancel_certainty(ring) ==
             TransferCertainty::FullyTransferred);

    backend->stop();
    runtime->stop();
}

void test_runtime_stop_cancels_and_drains_idempotently() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto backend_result = runtime->open_bulk_out(matching_device(runtime));
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);
    auto first = payload(4);
    auto second = payload(4);

    KB_CHECK(backend->submit(TransferSubmission{10, 0, *first, first}) ==
             SubmitResult::accepted);
    KB_CHECK(backend->submit(TransferSubmission{11, 4, *second, second}) ==
             SubmitResult::accepted);
    backend->cancel(10);
    runtime->stop();
    runtime->stop();

    KB_CHECK(!runtime->running());
    KB_CHECK(backend->in_flight() == 0);
    std::unordered_set<std::uint64_t> cancelled_ids;
    TransferCompletion completion;
    while (backend->try_pop_completion(completion)) {
        KB_CHECK(completion.code == CompletionCode::cancelled);
        cancelled_ids.insert(completion.id);
    }
    KB_CHECK(cancelled_ids == std::unordered_set<std::uint64_t>({10, 11}));
    KB_CHECK(fake->release_calls == 1);
    KB_CHECK(fake->close_calls == 1);
    KB_CHECK(fake->free_transfer_calls == 2);
    KB_CHECK(fake->exit_calls == 1);
    KB_CHECK(fake->pin_module_calls == 0);
    backend->stop();
}

void test_terminal_event_error_poisons_accepting() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    fake->queue_event_result(LIBUSB_ERROR_IO);
    wait_until([&] {
        return runtime->last_event_error() == std::optional<int>{LIBUSB_ERROR_IO};
    });

    const auto enumeration = runtime->enumerate(UsbInterfaceFilter{});
    KB_CHECK(!enumeration.has_value());
    KB_CHECK(enumeration.error().kind == LibusbRuntimeErrorKind::runtime_stopped);
    runtime->stop();
    KB_CHECK(!runtime->running());
    KB_CHECK(!runtime->shutdown_quarantined());
    KB_CHECK(fake->exit_calls == 1);
}

void test_explicit_zero_packet_contract() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);

    auto default_backend_result = runtime->open_bulk_out(snapshot);
    KB_CHECK(default_backend_result.has_value());
    auto default_backend = std::move(*default_backend_result);
    auto default_aligned = payload(8);
    KB_CHECK(default_backend->submit(
                 TransferSubmission{19, 0, *default_aligned, default_aligned, true}) ==
             SubmitResult::accepted);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 8);
    KB_CHECK(wait_for_completion(*default_backend).id == 19);
    KB_CHECK(fake->submission_count() == 1);
    default_backend->stop();

    const BulkOutOptions options{
        0, ZeroPacketPolicy::when_logical_message_end_packet_aligned};
    auto backend_result = runtime->open_bulk_out(snapshot, options);
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);

    // A packet-aligned ring chunk is not a logical-message boundary and must
    // complete without an added ZLP even under the opt-in policy.
    auto ring_chunk = payload(8);
    KB_CHECK(backend->submit(TransferSubmission{20, 0, *ring_chunk, ring_chunk}) ==
             SubmitResult::accepted);
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 8);
    KB_CHECK(wait_for_completion(*backend).id == 20);
    KB_CHECK(fake->submission_count() == 2);

    auto aligned = payload(8);
    KB_CHECK(backend->submit(TransferSubmission{21, 0, *aligned, aligned, true}) ==
             SubmitResult::accepted);
    KB_CHECK(fake->submission_count() == 3);
    const auto data_submission = fake->submission(2);
    KB_CHECK(data_submission.length == 8);
    KB_CHECK((data_submission.flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET) == 0);

    fake->complete_submission(2, LIBUSB_TRANSFER_COMPLETED, 8);
    TransferCompletion unexpected;
    wait_until([&] {
        KB_CHECK(!backend->try_pop_completion(unexpected));
        return fake->submission_count() == 4;
    });
    const auto zero_packet = fake->submission(3);
    KB_CHECK(zero_packet.length == 0);
    KB_CHECK(zero_packet.buffer == nullptr);

    fake->complete_submission(3, LIBUSB_TRANSFER_COMPLETED, 0);
    const auto aligned_completion = wait_for_completion(*backend);
    KB_CHECK(aligned_completion.id == 21);
    KB_CHECK(aligned_completion.code == CompletionCode::success);
    KB_CHECK(aligned_completion.transferred_bytes == 8);

    auto unaligned = payload(6);
    KB_CHECK(backend->submit(TransferSubmission{22, 0, *unaligned, unaligned, true}) ==
             SubmitResult::accepted);
    KB_CHECK(fake->submission_count() == 5);
    fake->complete_submission(4, LIBUSB_TRANSFER_COMPLETED, 6);
    const auto unaligned_completion = wait_for_completion(*backend);
    KB_CHECK(unaligned_completion.id == 22);
    KB_CHECK(unaligned_completion.code == CompletionCode::success);
    KB_CHECK(fake->submission_count() == 5);

    auto invalid_zero_packet = payload(4);
    KB_CHECK(backend->submit(
                 TransferSubmission{23,
                                    0,
                                    *invalid_zero_packet,
                                    invalid_zero_packet,
                                    true}) ==
             SubmitResult::accepted);
    fake->complete_submission(5, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_until([&] {
        KB_CHECK(!backend->try_pop_completion(unexpected));
        return fake->submission_count() == 7;
    });
    fake->complete_submission(6, LIBUSB_TRANSFER_COMPLETED, 1);
    const auto invalid_completion = wait_for_completion(*backend);
    KB_CHECK(invalid_completion.id == 23);
    KB_CHECK(invalid_completion.code == CompletionCode::io_error);

    backend->stop();
    runtime->stop();
}

void test_usb_transport_uses_process_budget_by_default() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    const auto process_budget =
        kairosboot::transport::process_usb_buffer_budget();
    KB_CHECK(process_budget->used() == 0);

    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    KB_CHECK(options.buffer_budget == nullptr);
    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto bytes = payload(8);
    auto pending = std::async(std::launch::async, [&] {
        return transport->write(*bytes, std::chrono::seconds(1));
    });

    wait_for_submissions(*fake, 2);
    KB_CHECK(process_budget->used() == 8);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto result = pending.get();
    KB_CHECK(result.status == TransportStatus::Ok);
    KB_CHECK(result.transferred == bytes->size());
    KB_CHECK(process_budget->used() == 0);
    const auto telemetry = transport->data_telemetry_snapshot();
    KB_CHECK(!telemetry.enabled);
    KB_CHECK(telemetry.source_read_count == 0);
    KB_CHECK(telemetry.submit_count == 0);
    KB_CHECK(telemetry.completion_count == 0);

    transport->close();
    runtime->stop();
}

void test_usb_data_transport_exposes_internal_telemetry_snapshot() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto device = matching_device(runtime);
    IncrementingTelemetryClock clock;
    clock.block_on_call = 5;
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    const auto budget = std::make_shared<BufferBudget>(8);
    auto held_budget = budget->try_acquire(8);
    KB_CHECK(held_budget.has_value());
    options.buffer_budget = budget;
    options.data_telemetry = TransferTelemetryConfig{
        .enabled = true,
        .clock = {
            .now = &sample_telemetry_clock,
            .context = &clock,
        },
    };

    auto opened = UsbFastbootTransport::open(runtime, device, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto bytes = payload(8);
    std::thread::id worker_thread;
    auto pending = std::async(std::launch::async, [&] {
        worker_thread = std::this_thread::get_id();
        return transport->write(*bytes, std::chrono::seconds(1));
    });

    wait_until([&] { return clock.blocked.load(std::memory_order_acquire); });
    held_budget.reset();
    clock.resume.store(true, std::memory_order_release);
    wait_for_submissions(*fake, 2);
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto result = pending.get();
    KB_CHECK(result.status == TransportStatus::Ok);

    const auto telemetry = transport->data_telemetry_snapshot();
    KB_CHECK(telemetry.enabled);
    KB_CHECK(telemetry.source_read_count == 2);
    KB_CHECK(telemetry.source_read_bytes == 8);
    KB_CHECK(telemetry.source_read_time == std::chrono::nanoseconds(2));
    KB_CHECK(telemetry.budget_acquire_attempt_count == 4);
    KB_CHECK(telemetry.budget_acquire_count == 2);
    KB_CHECK(telemetry.budget_acquire_time == std::chrono::nanoseconds(4));
    KB_CHECK(telemetry.budget_wait_count == 1);
    KB_CHECK(telemetry.budget_wait_time == std::chrono::nanoseconds(1));
    KB_CHECK(telemetry.submit_attempt_count == 2);
    KB_CHECK(telemetry.submit_count == 2);
    KB_CHECK(telemetry.submitted_bytes == 8);
    KB_CHECK(telemetry.completion_count == 2);
    KB_CHECK(telemetry.completed_bytes == 8);
    KB_CHECK(telemetry.current_in_flight == 0);
    KB_CHECK(telemetry.peak_in_flight == 2);
    KB_CHECK(telemetry.contiguous_watermark == 8);
    KB_CHECK(telemetry.cancel_count == 0);
    KB_CHECK(telemetry.backend_cancel_count == 0);
    KB_CHECK(telemetry.cancelled_completion_count == 0);
    KB_CHECK(telemetry.error_count == 0);
    KB_CHECK(clock.calls.load(std::memory_order_relaxed) == 14);
    KB_CHECK(clock.caller_thread == worker_thread);
    KB_CHECK(clock.caller_thread != runtime->event_thread_id());
    KB_CHECK(fake->callback_thread == runtime->event_thread_id());

    transport->close();
    runtime->stop();
}

void test_usb_logical_read_short_packet_zlp_and_overflow() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    options.buffer_budget = std::make_shared<BufferBudget>(8);

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    std::array<std::byte, 16> response{};
    auto pending_read = std::async(std::launch::async, [&] {
        return transport->read(response, std::chrono::seconds(1));
    });

    wait_for_submissions(*fake, 1);
    const auto first_read = fake->submission(0);
    KB_CHECK(first_read.endpoint == 0x81);
    KB_CHECK(first_read.length == 17);
    KB_CHECK(first_read.timeout > 0);
    fake->complete_in_submission(0, LIBUSB_TRANSFER_COMPLETED, {});

    // A transfer-level ZLP is consumed and the same logical read continues.
    wait_for_submissions(*fake, 2);
    const auto logical_response = ascii_bytes("OKAYdone");
    fake->complete_in_submission(
        1, LIBUSB_TRANSFER_COMPLETED, logical_response);
    const auto read = pending_read.get();
    KB_CHECK(read.status == TransportStatus::Ok);
    KB_CHECK(read.certainty == TransferCertainty::FullyTransferred);
    KB_CHECK(read.transferred == logical_response.size());
    KB_CHECK(!read.truncated);
    KB_CHECK(std::equal(logical_response.begin(),
                        logical_response.end(),
                        response.begin()));
    KB_CHECK(fake->callback_thread == runtime->event_thread_id());
    KB_CHECK(fake->callback_thread != std::this_thread::get_id());
    transport->close();

    auto overflow_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(overflow_opened.has_value());
    auto overflow_transport = std::move(*overflow_opened);
    std::array<std::byte, 4> limited{};
    auto overflow_read = std::async(std::launch::async, [&] {
        return overflow_transport->read(limited, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 3);
    const auto too_large = ascii_bytes("OKAYx");
    fake->complete_in_submission(
        2, LIBUSB_TRANSFER_COMPLETED, too_large);
    const auto truncated = overflow_read.get();
    KB_CHECK(truncated.status == TransportStatus::IoError);
    KB_CHECK(truncated.certainty == TransferCertainty::PartialOrUnknown);
    KB_CHECK(truncated.transferred == limited.size());
    KB_CHECK(truncated.truncated);
    KB_CHECK(!overflow_transport->is_open());

    auto native_overflow_opened =
        UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(native_overflow_opened.has_value());
    auto native_overflow_transport = std::move(*native_overflow_opened);
    std::array<std::byte, 8> native_limited{};
    auto native_overflow_read = std::async(std::launch::async, [&] {
        return native_overflow_transport->read(
            native_limited, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 4);
    const auto overflow_prefix = ascii_bytes("OKAYdata");
    fake->complete_in_submission(
        3, LIBUSB_TRANSFER_OVERFLOW, overflow_prefix, 8);
    const auto native_overflow = native_overflow_read.get();
    KB_CHECK(native_overflow.status == TransportStatus::IoError);
    KB_CHECK(native_overflow.truncated);
    KB_CHECK(native_overflow.transferred == native_limited.size());
    KB_CHECK(!native_overflow_transport->is_open());

    runtime->stop();
}

void test_usb_data_read_does_not_probe_past_destination() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    options.buffer_budget = std::make_shared<BufferBudget>(8);

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    std::array<std::byte, 4> payload{};
    auto pending_data = std::async(std::launch::async, [&] {
        return transport->read_data(payload, std::chrono::seconds(1));
    });

    wait_for_submissions(*fake, 1);
    const auto data_submission = fake->submission(0);
    KB_CHECK(data_submission.endpoint == 0x81);
    KB_CHECK(data_submission.length == 4);
    const auto raw = ascii_bytes("data");
    fake->complete_in_submission(0, LIBUSB_TRANSFER_COMPLETED, raw);
    const auto data = pending_data.get();
    KB_CHECK(data.status == TransportStatus::Ok);
    KB_CHECK(data.certainty == TransferCertainty::FullyTransferred);
    KB_CHECK(data.transferred == raw.size());
    KB_CHECK(!data.truncated);
    KB_CHECK(std::equal(raw.begin(), raw.end(), payload.begin()));
    KB_CHECK(transport->is_open());

    std::array<std::byte, 8> response{};
    auto pending_status = std::async(std::launch::async, [&] {
        return transport->read(response, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 2);
    const auto status_submission = fake->submission(1);
    KB_CHECK(status_submission.length == 9);
    const auto okay = ascii_bytes("OKAYdone");
    fake->complete_in_submission(1, LIBUSB_TRANSFER_COMPLETED, okay);
    const auto status = pending_status.get();
    KB_CHECK(status.status == TransportStatus::Ok);
    KB_CHECK(status.transferred == okay.size());
    KB_CHECK(!status.truncated);
    KB_CHECK(std::equal(okay.begin(), okay.end(), response.begin()));

    transport->close();
    runtime->stop();
}

void test_usb_read_error_classification_timeout_disconnect_stall_and_cancel() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 1};
    options.buffer_budget = std::make_shared<BufferBudget>(4);
    std::size_t submission_index = 0;

    fake->queue_submit_result(LIBUSB_ERROR_NO_DEVICE);
    auto submit_failure_opened =
        UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(submit_failure_opened.has_value());
    auto submit_failure_transport = std::move(*submit_failure_opened);
    std::array<std::byte, 16> submit_failure_response{};
    const auto submit_failure = submit_failure_transport->read(
        submit_failure_response, std::chrono::seconds(1));
    KB_CHECK(submit_failure.status == TransportStatus::Disconnected);
    KB_CHECK(submit_failure.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(fake->submission_count() == 0);

    const auto scripted_read = [&](const libusb_transfer_status status) {
        auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
        KB_CHECK(opened.has_value());
        auto transport = std::move(*opened);
        std::array<std::byte, 16> response{};
        auto read = std::async(std::launch::async, [&] {
            return transport->read(response, std::chrono::seconds(1));
        });
        wait_for_submissions(*fake, submission_index + 1U);
        fake->complete_in_submission(submission_index, status, {});
        ++submission_index;
        return read.get();
    };

    const auto timed_out = scripted_read(LIBUSB_TRANSFER_TIMED_OUT);
    KB_CHECK(timed_out.status == TransportStatus::Timeout);
    KB_CHECK(timed_out.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(timed_out.native_code == LIBUSB_ERROR_TIMEOUT);

    const auto disconnected = scripted_read(LIBUSB_TRANSFER_NO_DEVICE);
    KB_CHECK(disconnected.status == TransportStatus::Disconnected);
    KB_CHECK(disconnected.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(disconnected.native_code == LIBUSB_ERROR_NO_DEVICE);

    const auto stalled = scripted_read(LIBUSB_TRANSFER_STALL);
    KB_CHECK(stalled.status == TransportStatus::IoError);
    KB_CHECK(stalled.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(stalled.native_code == LIBUSB_ERROR_PIPE);

    auto cancel_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(cancel_opened.has_value());
    auto cancel_transport = std::move(*cancel_opened);
    std::array<std::byte, 16> cancelled_response{};
    auto cancelled_read = std::async(std::launch::async, [&] {
        return cancel_transport->read(
            cancelled_response, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, submission_index + 1U);
    fake->suppress_in_cancel_completion = true;
    cancel_transport->request_cancel();
    KB_CHECK(fake->cancel_calls >= 1);
    KB_CHECK(!runtime->shutdown_quarantined());
    fake->complete_in_submission(
        submission_index, LIBUSB_TRANSFER_CANCELLED, {});
    ++submission_index;
    const auto cancelled = cancelled_read.get();
    KB_CHECK(cancelled.status == TransportStatus::Cancelled);
    KB_CHECK(cancelled.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(cancelled.native_code == 0);
    KB_CHECK(!cancel_transport->is_open());
    fake->suppress_in_cancel_completion = false;

    auto pre_cancel_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(pre_cancel_opened.has_value());
    auto pre_cancel_transport = std::move(*pre_cancel_opened);
    pre_cancel_transport->request_cancel();
    const auto pre_cancelled = pre_cancel_transport->write(
        std::array{std::byte{1}}, std::chrono::seconds(1));
    KB_CHECK(pre_cancelled.status == TransportStatus::Cancelled);
    KB_CHECK(pre_cancelled.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(pre_cancelled.native_code == 0);

    runtime->stop();
}

void test_usb_ring_writes_are_serial_and_report_partial_certainty() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    options.buffer_budget = std::make_shared<BufferBudget>(8);

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto first_bytes = payload(8);
    const auto second_bytes = payload(4);
    auto first_write = std::async(std::launch::async, [&] {
        return transport->write(*first_bytes, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 2);
    KB_CHECK(fake->submission(0).endpoint == 0x01);
    KB_CHECK(fake->submission(1).endpoint == 0x01);

    auto second_write = std::async(std::launch::async, [&] {
        return transport->write(*second_bytes, std::chrono::seconds(1));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    KB_CHECK(fake->submission_count() == 2);

    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto first = first_write.get();
    KB_CHECK(first.status == TransportStatus::Ok);
    KB_CHECK(first.transferred == 8);
    KB_CHECK(first.certainty == TransferCertainty::FullyTransferred);

    wait_for_submissions(*fake, 3);
    fake->complete_submission(2, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto second = second_write.get();
    KB_CHECK(second.status == TransportStatus::Ok);
    KB_CHECK(second.transferred == 4);
    KB_CHECK(second.certainty == TransferCertainty::FullyTransferred);
    transport->close();

    auto partial_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(partial_opened.has_value());
    auto partial_transport = std::move(*partial_opened);
    const auto partial_bytes = payload(8);
    auto partial_write = std::async(std::launch::async, [&] {
        return partial_transport->write(
            *partial_bytes, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 5);
    fake->complete_submission(3, LIBUSB_TRANSFER_COMPLETED, 4);
    fake->complete_submission(4, LIBUSB_TRANSFER_COMPLETED, 2);
    const auto partial = partial_write.get();
    KB_CHECK(partial.status == TransportStatus::IoError);
    KB_CHECK(partial.transferred == 4);
    KB_CHECK(partial.certainty == TransferCertainty::PartialOrUnknown);
    KB_CHECK(!partial_transport->is_open());

    auto disconnect_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(disconnect_opened.has_value());
    auto disconnect_transport = std::move(*disconnect_opened);
    const auto disconnect_bytes = payload(4);
    auto disconnect_write = std::async(std::launch::async, [&] {
        return disconnect_transport->write(
            *disconnect_bytes, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 6);
    fake->complete_submission(5, LIBUSB_TRANSFER_NO_DEVICE, 0);
    const auto disconnected = disconnect_write.get();
    KB_CHECK(disconnected.status == TransportStatus::Disconnected);
    KB_CHECK(disconnected.certainty == TransferCertainty::PartialOrUnknown);
    KB_CHECK(!disconnect_transport->is_open());

    runtime->stop();
}

void test_usb_source_streams_beyond_ring_budget_with_ordered_progress() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    const auto process_budget =
        kairosboot::transport::process_usb_buffer_budget();
    KB_CHECK(process_budget->used() == 0);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    const auto budget = std::make_shared<BufferBudget>(8);
    options.buffer_budget = budget;

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto source = std::make_shared<PatternTransferSource>(19);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    std::atomic<std::size_t> progress_calls{0};
    std::thread::id worker_thread;
    std::thread::id observer_thread;

    auto pending = std::async(std::launch::async, [&] {
        worker_thread = std::this_thread::get_id();
        return transport->write_source(
            source,
            std::chrono::seconds(1),
            [&](const std::uint64_t watermark, const std::uint64_t total) {
                observer_thread = std::this_thread::get_id();
                progress.emplace_back(watermark, total);
                progress_calls.fetch_add(1, std::memory_order_release);
                return TransferProgressAction::continue_transfer;
            });
    });

    wait_for_submissions(*fake, 2);
    KB_CHECK(fake->submission(0).length == 4);
    KB_CHECK(fake->submission(1).length == 4);
    KB_CHECK(budget->used() == 8);
    KB_CHECK(process_budget->used() == 0);

    // Offset 4 completes first. The ring refills, but contiguous progress
    // remains at zero until the offset-0 gap closes.
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_for_submissions(*fake, 3);
    KB_CHECK(progress_calls.load(std::memory_order_acquire) == 0);
    KB_CHECK(fake->submission(2).length == 4);

    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_for_submissions(*fake, 4);
    wait_until([&] {
        return progress_calls.load(std::memory_order_acquire) == 1;
    });

    // Offset 12 now completes ahead of offset 8. The final irregular
    // three-byte source read is submitted without exceeding the ring budget.
    fake->complete_submission(3, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_for_submissions(*fake, 5);
    KB_CHECK(progress_calls.load(std::memory_order_acquire) == 1);
    KB_CHECK(fake->submission(4).length == 3);
    fake->complete_submission(2, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_until([&] {
        return progress_calls.load(std::memory_order_acquire) == 2;
    });
    fake->complete_submission(4, LIBUSB_TRANSFER_COMPLETED, 3);

    const auto result = pending.get();
    KB_CHECK(result.status == TransportStatus::Ok);
    KB_CHECK(result.transferred == 19);
    KB_CHECK(result.certainty == TransferCertainty::FullyTransferred);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected_progress{
        {8, 19}, {16, 19}, {19, 19}};
    KB_CHECK(progress == expected_progress);
    const std::array expected_reads{
        PatternTransferSource::Read{0, 4},
        PatternTransferSource::Read{4, 4},
        PatternTransferSource::Read{8, 4},
        PatternTransferSource::Read{12, 4},
        PatternTransferSource::Read{16, 3},
    };
    KB_CHECK(std::ranges::equal(source->reads(), expected_reads));
    KB_CHECK(budget->peak_used() == 8);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(process_budget->used() == 0);
    KB_CHECK(observer_thread == worker_thread);
    KB_CHECK(observer_thread != runtime->event_thread_id());
    KB_CHECK(fake->callback_thread == runtime->event_thread_id());

    transport->close();
    runtime->stop();
}

void test_usb_source_read_failure_poison_and_certainty() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    const auto budget = std::make_shared<BufferBudget>(8);
    options.buffer_budget = budget;

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto source = std::make_shared<PatternTransferSource>(12, 4);
    auto pending = std::async(std::launch::async, [&] {
        return transport->write_source(source, std::chrono::seconds(1));
    });

    wait_for_submissions(*fake, 1);
    const auto result = pending.get();
    KB_CHECK(result.status == TransportStatus::IoError);
    KB_CHECK(result.transferred == 0);
    KB_CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
    KB_CHECK(result.detail.find("source") != std::string::npos);
    const std::array expected_reads{
        PatternTransferSource::Read{0, 4},
        PatternTransferSource::Read{4, 4},
    };
    KB_CHECK(std::ranges::equal(source->reads(), expected_reads));
    KB_CHECK(fake->cancel_calls >= 1);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(!transport->is_open());

    runtime->stop();
}

void test_usb_source_progress_cancel_runs_on_owner_and_drains() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    const auto budget = std::make_shared<BufferBudget>(8);
    options.buffer_budget = budget;

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto source = std::make_shared<PatternTransferSource>(12);
    std::thread::id worker_thread;
    std::thread::id observer_thread;
    std::uint64_t observed_watermark{};
    std::uint64_t observed_total{};
    std::atomic<std::size_t> observer_calls{0};
    auto pending = std::async(std::launch::async, [&] {
        worker_thread = std::this_thread::get_id();
        return transport->write_source(
            source,
            std::chrono::seconds(1),
            [&](const std::uint64_t watermark, const std::uint64_t total) {
                observer_thread = std::this_thread::get_id();
                observed_watermark = watermark;
                observed_total = total;
                observer_calls.fetch_add(1, std::memory_order_release);
                return TransferProgressAction::cancel;
            });
    });

    wait_for_submissions(*fake, 2);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto result = pending.get();
    KB_CHECK(result.status == TransportStatus::Cancelled);
    KB_CHECK(result.transferred == 4);
    KB_CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
    KB_CHECK(result.native_code == 0);
    KB_CHECK(observer_calls.load(std::memory_order_acquire) == 1);
    KB_CHECK(observed_watermark == 4);
    KB_CHECK(observed_total == 12);
    KB_CHECK(observer_thread == worker_thread);
    KB_CHECK(observer_thread != runtime->event_thread_id());
    KB_CHECK(fake->callback_thread == runtime->event_thread_id());
    KB_CHECK(fake->cancel_calls >= 2);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(!transport->is_open());

    runtime->stop();
}

void test_usb_source_cross_thread_cancel_is_non_blocking_and_releases_resources() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 2};
    const auto budget = std::make_shared<BufferBudget>(8);
    options.buffer_budget = budget;

    auto opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(opened.has_value());
    auto transport = std::move(*opened);
    const auto source = std::make_shared<PatternTransferSource>(12);
    auto write = std::async(std::launch::async, [&] {
        return transport->write_source(source, std::chrono::seconds(5));
    });

    wait_for_submissions(*fake, 2);
    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_for_submissions(*fake, 3);

    // With native cancel callbacks suppressed, a cancel implementation that
    // waits for drain would block here. request_cancel() must only signal the
    // active backend and return independently of the serialized writer.
    fake->suppress_cancel_completion = true;
    std::promise<void> cancel_started;
    auto cancel_started_signal = cancel_started.get_future();
    auto cancel_call = std::async(std::launch::async, [&] {
        cancel_started.set_value();
        const auto started = std::chrono::steady_clock::now();
        transport->request_cancel();
        return std::chrono::steady_clock::now() - started;
    });
    cancel_started_signal.wait();
    KB_CHECK(cancel_call.wait_for(std::chrono::milliseconds(100)) ==
             std::future_status::ready);
    KB_CHECK(cancel_call.get() < std::chrono::milliseconds(100));
    KB_CHECK(write.wait_for(std::chrono::milliseconds::zero()) ==
             std::future_status::timeout);
    KB_CHECK(fake->cancel_calls >= 2);
    KB_CHECK(!transport->is_open());
    KB_CHECK(!runtime->shutdown_quarantined());

    const auto drain_started = std::chrono::steady_clock::now();
    fake->complete_submission(1, LIBUSB_TRANSFER_CANCELLED, 0);
    fake->complete_submission(2, LIBUSB_TRANSFER_CANCELLED, 0);
    KB_CHECK(write.wait_for(std::chrono::milliseconds(500)) ==
             std::future_status::ready);
    const auto result = write.get();
    KB_CHECK(std::chrono::steady_clock::now() - drain_started <
             std::chrono::milliseconds(500));
    KB_CHECK(result.status == TransportStatus::Cancelled);
    KB_CHECK(result.transferred == 4);
    KB_CHECK(result.certainty == TransferCertainty::PartialOrUnknown);
    KB_CHECK(result.native_code == 0);
    KB_CHECK(budget->used() == 0);
    KB_CHECK(fake->release_calls == 1);
    KB_CHECK(fake->close_calls == 1);
    KB_CHECK(!runtime->shutdown_quarantined());

    // The drained backend has released the physical-interface reservation, so
    // the same snapshot and shared budget can be safely opened again.
    auto reopened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(reopened.has_value());
    auto replacement = std::move(*reopened);
    replacement->close();
    KB_CHECK(budget->used() == 0);
    KB_CHECK(fake->release_calls == 2);
    KB_CHECK(fake->close_calls == 2);

    runtime->stop();
}

void test_usb_backend_timeout_releases_reservation_and_preserves_runtime() {
    auto fake = std::make_shared<FakeLibusb>();
    fake->suppress_in_cancel_completion = true;
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    UsbFastbootTransportOptions options;
    options.data_ring = TransferRingConfig{4, 1};
    options.buffer_budget = std::make_shared<BufferBudget>(4);

    auto first_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(first_opened.has_value());
    auto first = std::move(*first_opened);

    std::array<std::byte, 16> first_response{};
    auto first_read = std::async(std::launch::async, [&] {
        return first->read(first_response, std::chrono::milliseconds(20));
    });
    wait_for_submissions(*fake, 1);
    wait_until([&] { return fake->cancel_calls.load() != 0; });

    const auto first_result = first_read.get();
    KB_CHECK(first_result.status == TransportStatus::Timeout);
    KB_CHECK(first_result.certainty == TransferCertainty::NotTransferred);
    KB_CHECK(!first->is_open());
    KB_CHECK(runtime->running());
    KB_CHECK(!runtime->shutdown_quarantined());
    KB_CHECK(fake->pin_module_calls == 1);

    // The terminal backend is now rooted in the local quarantine. Its runtime
    // reservation is released only at that point, so a fresh session can open
    // while a late callback remains safely owned by the quarantined backend.
    auto second_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(second_opened.has_value());
    auto second = std::move(*second_opened);
    const auto second_payload = payload(4);
    auto second_write = std::async(std::launch::async, [&] {
        return second->write(*second_payload, std::chrono::seconds(1));
    });
    wait_for_submissions(*fake, 2);
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto second_result = second_write.get();
    KB_CHECK(second_result.status == TransportStatus::Ok);
    KB_CHECK(second_result.certainty == TransferCertainty::FullyTransferred);
    KB_CHECK(second->is_open());

    const auto duplicate = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(!duplicate.has_value());
    KB_CHECK(duplicate.error().kind == LibusbRuntimeErrorKind::interface_busy);
    second->close();

    // A normal close releases the reservation and permits the next open. The
    // late callback from the first backend is drained at runtime shutdown.
    auto third_opened = UsbFastbootTransport::open(runtime, snapshot, options);
    KB_CHECK(third_opened.has_value());
    auto third = std::move(*third_opened);
    third->close();
    fake->complete_in_submission(0, LIBUSB_TRANSFER_CANCELLED, {});
    runtime->stop();
    KB_CHECK(!runtime->shutdown_quarantined());
    KB_CHECK(fake->release_calls == 3);
    KB_CHECK(fake->close_calls == 3);
}

void test_backend_local_isolation_and_two_phase_global_quarantine() {
    auto fake = std::make_shared<FakeLibusb>();
    // Production fails fast if module pinning fails. The fake handler returns
    // so the failure state and ownership strategy can be inspected safely from
    // this main-executable test image.
    fake->pin_module_result = false;
    fake->suppress_cancel_completion = true;
    auto runtime = create_runtime(fake);
    const auto snapshot = matching_device(runtime);
    auto first_backend_result = runtime->open_bulk_out(snapshot);
    KB_CHECK(first_backend_result.has_value());
    auto first_backend = std::move(*first_backend_result);
    auto bytes = payload(4);
    KB_CHECK(first_backend->submit(TransferSubmission{30, 0, *bytes, bytes}) ==
             SubmitResult::accepted);

    const auto backend_stop_started = std::chrono::steady_clock::now();
    first_backend->stop();
    const auto backend_stop_elapsed =
        std::chrono::steady_clock::now() - backend_stop_started;
    // This is a deterministic fake envelope around the backend wait only; it
    // is not a bound on a real native cancel call.
    KB_CHECK(backend_stop_elapsed < std::chrono::milliseconds(600));
    KB_CHECK(runtime->running());
    KB_CHECK(!runtime->shutdown_quarantined());
    KB_CHECK(runtime->quarantine_module_pin_failed());
    KB_CHECK(first_backend->shutdown_quarantined());
    KB_CHECK(first_backend->in_flight() == 1);
    KB_CHECK(fake->pin_module_calls == 1);
    KB_CHECK(fake->pin_module_failure_calls == 1);

    // A backend-local timeout must not poison the shared runtime. A second
    // device backend can still be opened and complete new work.
    fake->suppress_cancel_completion = false;
    auto second_backend_result = runtime->open_bulk_out(snapshot);
    KB_CHECK(second_backend_result.has_value());
    auto second_backend = std::move(*second_backend_result);
    auto second_bytes = payload(4);
    KB_CHECK(second_backend->submit(
                 TransferSubmission{31, 0, *second_bytes, second_bytes}) ==
             SubmitResult::accepted);
    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 4);
    const auto second_completion = wait_for_completion(*second_backend);
    KB_CHECK(second_completion.id == 31);
    KB_CHECK(second_completion.code == CompletionCode::success);
    KB_CHECK(fake->free_transfer_calls == 1);

    fake->request_event_block();
    fake->wait_for_event_block_start();
    runtime->stop();
    // The fake native event call is still latched. Returning from stop before
    // explicitly releasing it proves that the bounded drain and event-exit
    // phases quarantined the runtime instead of waiting on the native call.
    KB_CHECK(!fake->blocked_event_completed.load(std::memory_order_acquire));
    KB_CHECK(!runtime->running());
    KB_CHECK(runtime->shutdown_quarantined());
    KB_CHECK(first_backend->in_flight() == 1);
    KB_CHECK(!second_backend->shutdown_quarantined());
    KB_CHECK(fake->cancel_calls >= 1);
    KB_CHECK(fake->free_transfer_calls == 1);
    KB_CHECK(fake->release_calls == 1);
    KB_CHECK(fake->close_calls == 1);
    KB_CHECK(fake->exit_calls == 0);
    KB_CHECK(fake->pin_module_calls == 1);

    fake->release_event_block();
    wait_until([&] {
        return fake->blocked_event_completed.load(std::memory_order_acquire);
    });
    const auto second_stop_started = std::chrono::steady_clock::now();
    runtime->stop();
    first_backend->stop();
    second_backend->stop();
    KB_CHECK(std::chrono::steady_clock::now() - second_stop_started <
             std::chrono::milliseconds(100));
    KB_CHECK(fake->free_transfer_calls == 1);

    auto replacement_fake = std::make_shared<FakeLibusb>();
    const auto replacement = LibusbRuntime::create(replacement_fake->functions());
    KB_CHECK(!replacement.has_value());
    KB_CHECK(replacement.error().kind == LibusbRuntimeErrorKind::already_running);
    KB_CHECK(replacement_fake->init_calls == 0);
}

struct TestCase final {
    std::string_view name;
    std::function<void()> run;
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"init failure, version, and singleton", test_init_failure_version_and_singleton},
        {"event loop and filtered UTF-8 enumeration", test_event_loop_and_filtered_utf8_enumeration},
        {"Linux topology enrichment and diagnostic",
         test_enumeration_retains_linux_topology_or_diagnostic},
        {"macOS topology enrichment and diagnostic",
         test_enumeration_retains_macos_topology_or_diagnostic},
        {"platform topology device/interface identity",
         test_device_topology_is_resolved_once_for_distinct_interfaces},
        {"Windows topology exact session and zero rejection",
         test_zero_windows_session_is_diagnostic_and_never_resolved},
        {"Windows topology session identity capture diagnostic",
         test_windows_session_identity_capture_failure_is_diagnostic},
        {"Windows DEVINST generation reuse",
         test_windows_devinst_reuse_never_publishes_stale_libusb_identity},
        {"Windows stale address and serial generation gate",
         test_windows_generation_revalidation_rejects_stale_address_and_serial},
        {"macOS topology stop cancellation",
         test_runtime_stop_cancels_macos_topology_outside_lifecycle_lock},
        {"open physical identity and address reuse", test_open_revalidates_physical_identity_and_address_reuse},
        {"open rejects changed interface snapshot", test_open_rejects_changed_interface_snapshot},
        {"open configuration before claim", test_open_configuration_is_verified_before_claim},
        {"configuration failure releases reservation",
         test_open_configuration_failures_release_reservation},
        {"runtime reservation and claim busy",
         test_runtime_reservation_and_claim_busy_contract},
        {"out-of-order completion and payload lifetime", test_out_of_order_completion_and_payload_lifetime},
        {"submit and completion error classification", test_submit_and_completion_error_classification},
        {"submit allocation failures", test_submit_allocation_failures_do_not_throw_or_leak},
        {"transfer ring adapter flow", test_transfer_ring_adapter_lifetime_and_completion_flow},
        {"observer cancel certainty for submitted and out-of-order data",
         test_observer_cancel_certainty_includes_submitted_and_out_of_order_data},
        {"runtime stop cancel and drain", test_runtime_stop_cancels_and_drains_idempotently},
        {"terminal event error poisons accepting", test_terminal_event_error_poisons_accepting},
        {"explicit zero packet contract", test_explicit_zero_packet_contract},
        {"USB default process budget",
         test_usb_transport_uses_process_budget_by_default},
        {"USB DATA internal telemetry snapshot",
         test_usb_data_transport_exposes_internal_telemetry_snapshot},
        {"USB logical read ZLP, short packet, overflow",
         test_usb_logical_read_short_packet_zlp_and_overflow},
        {"USB DATA read does not over-read",
         test_usb_data_read_does_not_probe_past_destination},
        {"USB read error classification",
         test_usb_read_error_classification_timeout_disconnect_stall_and_cancel},
        {"USB ring writes serialize and report certainty",
         test_usb_ring_writes_are_serial_and_report_partial_certainty},
        {"USB source streaming and ordered progress",
         test_usb_source_streams_beyond_ring_budget_with_ordered_progress},
        {"USB source read failure certainty",
         test_usb_source_read_failure_poison_and_certainty},
        {"USB source progress cancellation",
         test_usb_source_progress_cancel_runs_on_owner_and_drains},
        {"USB source cross-thread cancellation",
         test_usb_source_cross_thread_cancel_is_non_blocking_and_releases_resources},
        {"USB backend timeout reservation lifecycle",
         test_usb_backend_timeout_releases_reservation_and_preserves_runtime},
        {"backend isolation and two-phase quarantine", test_backend_local_isolation_and_two_phase_global_quarantine},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
