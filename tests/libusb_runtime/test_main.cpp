#include "src/transport/libusb_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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
using kairosboot::transport::MemoryTransferSource;
using kairosboot::transport::SubmitResult;
using kairosboot::transport::TransferCompletion;
using kairosboot::transport::TransferRing;
using kairosboot::transport::TransferRingConfig;
using kairosboot::transport::TransferRingState;
using kairosboot::transport::TransferSubmission;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::UsbInterfaceFilter;
using kairosboot::transport::ZeroPacketPolicy;

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

class FakeLibusb final : public std::enable_shared_from_this<FakeLibusb> {
public:
    struct Event final {
        libusb_transfer* transfer{};
        libusb_transfer_status status{LIBUSB_TRANSFER_ERROR};
        int actual_length{};
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
                                                    libusb_config_descriptor** config) {
            *config = &self->config_;
            return LIBUSB_SUCCESS;
        };
        table.get_config_descriptor = [self](libusb_device*,
                                             std::uint8_t,
                                             libusb_config_descriptor** config) {
            *config = &self->config_;
            return LIBUSB_SUCCESS;
        };
        table.free_config_descriptor = [self](libusb_config_descriptor*) {
            ++self->free_config_calls;
        };
        table.get_bus_number = [](libusb_device*) { return std::uint8_t{2}; };
        table.get_device_address = [](libusb_device*) { return std::uint8_t{5}; };
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
        table.claim_interface = [self](libusb_device_handle*, int) {
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
            self->events_.push_back(
                Event{transfer, LIBUSB_TRANSFER_CANCELLED, self->cancel_actual_length});
            self->event_cv_.notify_all();
            return LIBUSB_SUCCESS;
        };
        table.free_transfer = [self](libusb_transfer* transfer) {
            ++self->free_transfer_calls;
            std::free(transfer);
        };
        return table;
    }

    void queue_submit_result(const int result) {
        std::lock_guard lock(mutex_);
        submit_results_.push_back(result);
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

    [[nodiscard]] std::size_t submission_count() const {
        std::lock_guard lock(mutex_);
        return submissions_.size();
    }

    [[nodiscard]] libusb_transfer submission(const std::size_t index) const {
        std::lock_guard lock(mutex_);
        return *submissions_.at(index);
    }

    void wait_for_event_loop() {
        wait_until([this] { return handle_event_calls.load() != 0; });
    }

    libusb_version version_{};
    int init_result{LIBUSB_SUCCESS};
    int open_result{LIBUSB_SUCCESS};
    int claim_result{LIBUSB_SUCCESS};
    int alternate_result{LIBUSB_SUCCESS};
    int cancel_actual_length{};
    bool fail_allocation{false};
    std::atomic<int> init_calls{0};
    std::atomic<int> exit_calls{0};
    std::atomic<int> handle_event_calls{0};
    std::atomic<int> free_device_list_calls{0};
    std::atomic<int> free_config_calls{0};
    std::atomic<int> open_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> claim_calls{0};
    std::atomic<int> release_calls{0};
    std::atomic<int> alternate_calls{0};
    std::atomic<int> cancel_calls{0};
    std::atomic<int> free_transfer_calls{0};
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
            return interrupted_ || !events_.empty();
        });
        if (interrupted_) {
            interrupted_ = false;
            return LIBUSB_ERROR_INTERRUPTED;
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
    std::deque<Event> events_;
    std::deque<int> submit_results_;
    std::vector<libusb_transfer*> submissions_;
    std::unordered_set<libusb_transfer*> active_;
    std::unordered_set<libusb_transfer*> cancel_queued_;
};

[[nodiscard]] std::shared_ptr<LibusbRuntime> create_runtime(
    const std::shared_ptr<FakeLibusb>& fake) {
    auto runtime = LibusbRuntime::create(fake->functions());
    KB_CHECK(runtime.has_value());
    fake->wait_for_event_loop();
    return *runtime;
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

void test_init_failure_version_and_singleton() {
    const auto system_functions = LibusbFunctions::system();
    KB_CHECK(system_functions.complete());
    const auto* linked_version = system_functions.get_version();
    KB_CHECK(linked_version != nullptr);
    KB_CHECK(linked_version->major == 1);
    KB_CHECK(linked_version->minor == 0);
    KB_CHECK(linked_version->micro == 30);

    const auto incomplete = LibusbRuntime::create(LibusbFunctions{});
    KB_CHECK(!incomplete.has_value());
    KB_CHECK(incomplete.error().kind == LibusbRuntimeErrorKind::invalid_function_table);

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

    const auto device = matching_device(runtime);
    KB_CHECK(device.vendor_id == 0x18D1);
    KB_CHECK(device.product_id == 0x4EE0);
    KB_CHECK(device.bus_number == 2);
    KB_CHECK(device.device_address == 5);
    KB_CHECK(device.port_path == std::vector<std::uint8_t>({3, 4}));
    KB_CHECK(device.serial_utf8 == "serial-\xCE\xB1");
    KB_CHECK(device.interface_number == 2);
    KB_CHECK(device.bulk_out_endpoint == 0x01);
    KB_CHECK(device.bulk_out_max_packet_size == 4);

    UsbInterfaceFilter mismatch;
    mismatch.interface_protocol = 0x99;
    const auto empty = runtime->enumerate(mismatch);
    KB_CHECK(empty.has_value());
    KB_CHECK(empty->empty());
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

void test_transfer_ring_adapter_lifetime_and_completion_flow() {
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
    backend->stop();
}

void test_explicit_zero_packet_contract() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    const BulkOutOptions options{0, ZeroPacketPolicy::when_packet_aligned};
    auto backend_result = runtime->open_bulk_out(matching_device(runtime), options);
    KB_CHECK(backend_result.has_value());
    auto backend = std::move(*backend_result);

    auto aligned = payload(8);
    KB_CHECK(backend->submit(TransferSubmission{20, 0, *aligned, aligned}) ==
             SubmitResult::accepted);
    KB_CHECK(fake->submission_count() == 1);
    const auto data_submission = fake->submission(0);
    KB_CHECK(data_submission.length == 8);
    KB_CHECK((data_submission.flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET) == 0);

    fake->complete_submission(0, LIBUSB_TRANSFER_COMPLETED, 8);
    TransferCompletion unexpected;
    wait_until([&] {
        KB_CHECK(!backend->try_pop_completion(unexpected));
        return fake->submission_count() == 2;
    });
    const auto zero_packet = fake->submission(1);
    KB_CHECK(zero_packet.length == 0);
    KB_CHECK(zero_packet.buffer == nullptr);

    fake->complete_submission(1, LIBUSB_TRANSFER_COMPLETED, 0);
    const auto aligned_completion = wait_for_completion(*backend);
    KB_CHECK(aligned_completion.id == 20);
    KB_CHECK(aligned_completion.code == CompletionCode::success);
    KB_CHECK(aligned_completion.transferred_bytes == 8);

    auto unaligned = payload(6);
    KB_CHECK(backend->submit(TransferSubmission{21, 0, *unaligned, unaligned}) ==
             SubmitResult::accepted);
    KB_CHECK(fake->submission_count() == 3);
    fake->complete_submission(2, LIBUSB_TRANSFER_COMPLETED, 6);
    const auto unaligned_completion = wait_for_completion(*backend);
    KB_CHECK(unaligned_completion.id == 21);
    KB_CHECK(unaligned_completion.code == CompletionCode::success);
    KB_CHECK(fake->submission_count() == 3);

    auto invalid_zero_packet = payload(4);
    KB_CHECK(backend->submit(
                 TransferSubmission{22, 0, *invalid_zero_packet, invalid_zero_packet}) ==
             SubmitResult::accepted);
    fake->complete_submission(3, LIBUSB_TRANSFER_COMPLETED, 4);
    wait_until([&] {
        KB_CHECK(!backend->try_pop_completion(unexpected));
        return fake->submission_count() == 5;
    });
    fake->complete_submission(4, LIBUSB_TRANSFER_COMPLETED, 1);
    const auto invalid_completion = wait_for_completion(*backend);
    KB_CHECK(invalid_completion.id == 22);
    KB_CHECK(invalid_completion.code == CompletionCode::io_error);

    backend->stop();
    runtime->stop();
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
        {"out-of-order completion and payload lifetime", test_out_of_order_completion_and_payload_lifetime},
        {"submit and completion error classification", test_submit_and_completion_error_classification},
        {"transfer ring adapter flow", test_transfer_ring_adapter_lifetime_and_completion_flow},
        {"runtime stop cancel and drain", test_runtime_stop_cancels_and_drains_idempotently},
        {"explicit zero packet contract", test_explicit_zero_packet_contract},
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
