// SPDX-License-Identifier: MIT
#include "src/fastboot/libusb_reconnect_adapters.hpp"

#include "src/fastboot/primitive_service.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace kairosboot::fastboot {
namespace {

[[nodiscard]] bool valid_text(const std::string_view value) noexcept {
    return !value.empty() &&
        std::ranges::none_of(value, [](const unsigned char character) {
            return character < 0x20U || character == 0x7FU;
        });
}

[[nodiscard]] bool valid_optional_text(
    const std::optional<std::string>& value) noexcept {
    return !value.has_value() || valid_text(*value);
}

[[nodiscard]] bool valid_ports(
    const std::span<const std::uint8_t> ports) noexcept {
    return !ports.empty() && ports.size() <= 7U &&
        std::ranges::all_of(ports, [](const std::uint8_t port) {
            return port != 0U;
        });
}

[[nodiscard]] ReconnectUsbFingerprint reconnect_fingerprint(
    const transport::UsbDeviceInfo& device) noexcept {
    return ReconnectUsbFingerprint{
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .interface_number = device.interface_number,
        .interface_class = device.interface_class,
        .interface_subclass = device.interface_subclass,
        .interface_protocol = device.interface_protocol,
    };
}

[[nodiscard]] bool valid_fingerprint(
    const ReconnectUsbFingerprint& fingerprint) noexcept {
    return fingerprint.vendor_id != 0U &&
        fingerprint.interface_class == 0xFFU &&
        fingerprint.interface_subclass == 0x42U &&
        fingerprint.interface_protocol == 0x03U;
}

[[nodiscard]] bool same_candidate(const ReconnectCandidate& left,
                                  const ReconnectCandidate& right) noexcept {
    return left.physical_port == right.physical_port &&
        left.serial == right.serial &&
        left.usb_fingerprint == right.usb_fingerprint;
}

[[nodiscard]] protocol::TransferCertainty aggregate_certainty(
    const protocol::TransferCertainty aggregate,
    const protocol::TransferCertainty next) noexcept {
    if (aggregate == protocol::TransferCertainty::PartialOrUnknown ||
        next == protocol::TransferCertainty::PartialOrUnknown) {
        return protocol::TransferCertainty::PartialOrUnknown;
    }
    if (aggregate == protocol::TransferCertainty::FullyTransferred ||
        next == protocol::TransferCertainty::FullyTransferred) {
        return protocol::TransferCertainty::FullyTransferred;
    }
    return protocol::TransferCertainty::NotTransferred;
}

[[nodiscard]] std::string runtime_error_message(
    const transport::LibusbRuntimeErrorKind kind) {
    using transport::LibusbRuntimeErrorKind;
    switch (kind) {
        case LibusbRuntimeErrorKind::runtime_stopped:
            return "libusb runtime is stopped";
        case LibusbRuntimeErrorKind::enumeration_failed:
            return "libusb passive enumeration failed";
        case LibusbRuntimeErrorKind::invalid_device:
            return "libusb reconnect candidate is invalid";
        case LibusbRuntimeErrorKind::device_not_found:
            return "libusb reconnect candidate is no longer present";
        case LibusbRuntimeErrorKind::open_failed:
            return "libusb could not open the reconnect candidate";
        case LibusbRuntimeErrorKind::configuration_failed:
            return "libusb could not select the reconnect configuration";
        case LibusbRuntimeErrorKind::interface_busy:
            return "libusb reconnect interface is busy";
        case LibusbRuntimeErrorKind::claim_failed:
            return "libusb could not claim the reconnect interface";
        case LibusbRuntimeErrorKind::alternate_setting_failed:
            return "libusb could not select the reconnect alternate setting";
        case LibusbRuntimeErrorKind::operation_cancelled:
            return "libusb reconnect operation was cancelled";
        case LibusbRuntimeErrorKind::operation_timed_out:
            return "libusb reconnect operation deadline expired";
        case LibusbRuntimeErrorKind::identity_changed:
            return "USB identity changed during reconnect open";
        case LibusbRuntimeErrorKind::invalid_function_table:
        case LibusbRuntimeErrorKind::version_mismatch:
        case LibusbRuntimeErrorKind::already_running:
        case LibusbRuntimeErrorKind::init_failed:
        case LibusbRuntimeErrorKind::event_thread_failed:
        case LibusbRuntimeErrorKind::event_loop_failed:
            return "libusb runtime is unavailable";
    }
    return "libusb reconnect operation failed";
}

[[nodiscard]] bool retryable_open_error(
    const transport::LibusbRuntimeErrorKind kind) noexcept {
    using transport::LibusbRuntimeErrorKind;
    switch (kind) {
        case LibusbRuntimeErrorKind::device_not_found:
        case LibusbRuntimeErrorKind::open_failed:
        case LibusbRuntimeErrorKind::interface_busy:
        case LibusbRuntimeErrorKind::identity_changed:
            return true;
        case LibusbRuntimeErrorKind::invalid_function_table:
        case LibusbRuntimeErrorKind::version_mismatch:
        case LibusbRuntimeErrorKind::already_running:
        case LibusbRuntimeErrorKind::init_failed:
        case LibusbRuntimeErrorKind::event_thread_failed:
        case LibusbRuntimeErrorKind::event_loop_failed:
        case LibusbRuntimeErrorKind::runtime_stopped:
        case LibusbRuntimeErrorKind::enumeration_failed:
        case LibusbRuntimeErrorKind::invalid_device:
        case LibusbRuntimeErrorKind::configuration_failed:
        case LibusbRuntimeErrorKind::claim_failed:
        case LibusbRuntimeErrorKind::alternate_setting_failed:
        case LibusbRuntimeErrorKind::operation_cancelled:
        case LibusbRuntimeErrorKind::operation_timed_out:
            return false;
    }
    return false;
}

[[nodiscard]] ReconnectOpenError open_runtime_error(
    const transport::LibusbRuntimeError& error) {
    return ReconnectOpenError{
        .message = runtime_error_message(error.kind),
        .native_code = error.native_code,
        .retryable = retryable_open_error(error.kind),
        .outbound_certainty =
            protocol::TransferCertainty::NotTransferred,
    };
}

class ProbeInterruptionGuard final {
public:
    enum class Status : std::uint8_t {
        Clear,
        Cancelled,
        DeadlineExpired,
    };

    ProbeInterruptionGuard(protocol::FastbootSession& session,
                           const ReconnectTimePoint deadline,
                           const std::stop_token cancellation)
        : session_(session),
          cancellation_(cancellation, CancelProbe{this}),
          deadline_thread_([this, deadline](const std::stop_token stop) {
              std::unique_lock lock(mutex_);
              const bool finished = condition_.wait_until(
                  lock, stop, deadline, [this] { return finished_; });
              if (!finished) {
                  deadline_fired_.store(true, std::memory_order_release);
                  session_.request_cancel();
              }
          }) {}

    ProbeInterruptionGuard(const ProbeInterruptionGuard&) = delete;
    ProbeInterruptionGuard& operator=(const ProbeInterruptionGuard&) = delete;

    ~ProbeInterruptionGuard() {
        {
            const std::lock_guard lock(mutex_);
            finished_ = true;
        }
        deadline_thread_.request_stop();
        condition_.notify_all();
    }

    [[nodiscard]] bool deadline_fired() const noexcept {
        return deadline_fired_.load(std::memory_order_acquire);
    }

    // Linearizes the handoff against both interruption callbacks. Once Clear
    // is returned, neither the stop callback nor the deadline thread can set
    // the session's sticky cancellation bit; later cancellation belongs to
    // the caller that owns the published session.
    [[nodiscard]] Status finish(
        const ReconnectTimePoint deadline,
        const std::stop_token cancellation) noexcept {
        Status status = Status::Clear;
        {
            const std::lock_guard lock(mutex_);
            if (cancellation.stop_requested()) {
                status = Status::Cancelled;
            } else if (deadline_fired_.load(std::memory_order_acquire) ||
                       ReconnectClock::now() >= deadline) {
                status = Status::DeadlineExpired;
            }
            finished_ = true;
            // Close the check/publish window for a stop request that raced the
            // first check while its callback was waiting for mutex_. The
            // callback observes finished_ and therefore cannot poison session_.
            if (status == Status::Clear && cancellation.stop_requested()) {
                status = Status::Cancelled;
            }
        }
        deadline_thread_.request_stop();
        condition_.notify_all();
        return status;
    }

private:
    struct CancelProbe final {
        ProbeInterruptionGuard* guard{};

        void operator()() const noexcept { guard->cancel(); }
    };

    void cancel() noexcept {
        const std::lock_guard lock(mutex_);
        if (!finished_) {
            session_.request_cancel();
        }
    }

    protocol::FastbootSession& session_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool finished_{};
    std::atomic<bool> deadline_fired_{};
    std::stop_callback<CancelProbe> cancellation_;
    std::jthread deadline_thread_;
};

[[nodiscard]] std::optional<ReconnectOpenError> interrupted_probe(
    const ProbeInterruptionGuard& guard,
    const ReconnectTimePoint deadline,
    const std::stop_token cancellation,
    const protocol::TransferCertainty certainty) {
    if (cancellation.stop_requested()) {
        return ReconnectOpenError{
            .message = "Fastboot reconnect identity probe was cancelled",
            .native_code = 0,
            .retryable = false,
            .outbound_certainty = certainty,
        };
    }
    if (guard.deadline_fired() || ReconnectClock::now() >= deadline) {
        return ReconnectOpenError{
            .message = "Fastboot reconnect identity probe deadline expired",
            .native_code = LIBUSB_ERROR_TIMEOUT,
            .retryable = false,
            .outbound_certainty = certainty,
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ReconnectOpenError> finish_probe(
    ProbeInterruptionGuard& guard,
    const ReconnectTimePoint deadline,
    const std::stop_token cancellation,
    const protocol::TransferCertainty certainty) {
    switch (guard.finish(deadline, cancellation)) {
        case ProbeInterruptionGuard::Status::Clear:
            return std::nullopt;
        case ProbeInterruptionGuard::Status::Cancelled:
            return ReconnectOpenError{
                .message =
                    "Fastboot reconnect identity probe was cancelled before session handoff",
                .native_code = 0,
                .retryable = false,
                .outbound_certainty = certainty,
            };
        case ProbeInterruptionGuard::Status::DeadlineExpired:
            return ReconnectOpenError{
                .message =
                    "Fastboot reconnect identity probe deadline expired before session handoff",
                .native_code = LIBUSB_ERROR_TIMEOUT,
                .retryable = false,
                .outbound_certainty = certainty,
            };
    }
    return ReconnectOpenError{
        .message = "Fastboot reconnect interruption state is invalid",
        .native_code = LIBUSB_ERROR_OTHER,
        .retryable = false,
        .outbound_certainty = certainty,
    };
}

[[nodiscard]] ReconnectOpenError probe_error(
    fastboot::PrimitiveError error,
    const protocol::TransferCertainty prior_certainty,
    const ProbeInterruptionGuard& guard,
    const ReconnectTimePoint deadline,
    const std::stop_token cancellation) {
    const auto certainty =
        aggregate_certainty(prior_certainty, error.outbound_certainty);
    if (const auto interrupted =
            interrupted_probe(guard, deadline, cancellation, certainty)) {
        return *interrupted;
    }
    return ReconnectOpenError{
        .message = std::move(error.message),
        .native_code = error.native_code,
        .retryable = false,
        .outbound_certainty = certainty,
    };
}

[[nodiscard]] bool valid_adapter_options(
    const LibusbReconnectAdapterOptions& options) noexcept {
    const auto chunk = options.transport.data_ring.chunk_size;
    const auto depth = options.transport.data_ring.depth;
    return chunk != 0U && depth != 0U &&
        chunk <= std::numeric_limits<std::size_t>::max() / depth &&
        options.protocol.io_timeout > std::chrono::milliseconds::zero();
}

}  // namespace

class LibusbReconnectAdapter::OwnerIdentity final {};

class LibusbReconnectAdapter::CandidateOpenCapability final
    : public ReconnectCandidateOpenCapability {
public:
    CandidateOpenCapability(
        std::shared_ptr<const OwnerIdentity> owner,
        const ReconnectCandidate& candidate,
        transport::UsbDeviceInfo snapshot)
        : owner_(std::move(owner)),
          physical_port_(candidate.physical_port),
          serial_(candidate.serial),
          usb_fingerprint_(candidate.usb_fingerprint),
          snapshot_(std::move(snapshot)) {}

    [[nodiscard]] bool belongs_to(
        const std::shared_ptr<const OwnerIdentity>& owner,
        const ReconnectCandidate& candidate) const noexcept {
        return owner_ == owner && physical_port_ == candidate.physical_port &&
            serial_ == candidate.serial &&
            usb_fingerprint_ == candidate.usb_fingerprint;
    }

    [[nodiscard]] std::optional<transport::UsbDeviceInfo> consume() const {
        const std::lock_guard lock(mutex_);
        if (consumed_) {
            return std::nullopt;
        }
        consumed_ = true;
        return snapshot_;
    }

private:
    std::shared_ptr<const OwnerIdentity> owner_;
    UsbPhysicalPortPath physical_port_;
    std::optional<std::string> serial_;
    ReconnectUsbFingerprint usb_fingerprint_;
    transport::UsbDeviceInfo snapshot_;
    mutable std::mutex mutex_;
    mutable bool consumed_{};
};

LibusbReconnectAdapter::LibusbReconnectAdapter(
    std::shared_ptr<transport::LibusbRuntime> runtime,
    LibusbReconnectAdapterOptions options,
    std::shared_ptr<const OwnerIdentity> owner_identity) noexcept
    : runtime_(std::move(runtime)),
      options_(std::move(options)),
      owner_identity_(std::move(owner_identity)) {}

std::expected<std::unique_ptr<LibusbReconnectAdapter>,
              LibusbReconnectAdapterFactoryError>
LibusbReconnectAdapter::create(
    std::shared_ptr<transport::LibusbRuntime> runtime,
    LibusbReconnectAdapterOptions options) noexcept {
    try {
        if (runtime == nullptr || !runtime->running() ||
            !valid_adapter_options(options)) {
            return std::unexpected(LibusbReconnectAdapterFactoryError{
                .code =
                    LibusbReconnectAdapterFactoryErrorCode::InvalidArgument,
                .message = "libusb reconnect adapter options are invalid",
            });
        }
        return std::unique_ptr<LibusbReconnectAdapter>(
            new LibusbReconnectAdapter(std::move(runtime),
                                       std::move(options),
                                       std::make_shared<OwnerIdentity>()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(LibusbReconnectAdapterFactoryError{
            .code = LibusbReconnectAdapterFactoryErrorCode::ResourceExhausted,
            .message = {},
        });
    } catch (...) {
        return std::unexpected(LibusbReconnectAdapterFactoryError{
            .code = LibusbReconnectAdapterFactoryErrorCode::UnexpectedFailure,
            .message = {},
        });
    }
}

std::expected<std::vector<ReconnectCandidate>, ReconnectDiscoveryError>
LibusbReconnectAdapter::discover(
    const ReconnectTimePoint deadline,
    const std::stop_token cancellation) {
    const auto fail = [](ReconnectDiscoveryError error)
        -> std::expected<std::vector<ReconnectCandidate>,
                         ReconnectDiscoveryError> {
        return std::unexpected(std::move(error));
    };
    try {
        if (cancellation.stop_requested()) {
            return fail(ReconnectDiscoveryError{
                .message = "libusb reconnect discovery was cancelled",
                .native_code = 0,
                .retryable = false,
            });
        }
        if (ReconnectClock::now() >= deadline) {
            return fail(ReconnectDiscoveryError{
                .message = "libusb reconnect discovery deadline expired",
                .native_code = LIBUSB_ERROR_TIMEOUT,
                .retryable = false,
            });
        }

        transport::UsbInterfaceFilter filter;
        filter.interface_class = 0xFFU;
        filter.interface_subclass = 0x42U;
        filter.interface_protocol = 0x03U;
        auto snapshot = runtime_->enumerate(filter, deadline, cancellation);
        if (!snapshot.has_value()) {
            return fail(ReconnectDiscoveryError{
                .message = runtime_error_message(snapshot.error().kind),
                .native_code = snapshot.error().native_code,
                .retryable = snapshot.error().kind ==
                    transport::LibusbRuntimeErrorKind::enumeration_failed,
            });
        }
        if (cancellation.stop_requested() ||
            ReconnectClock::now() >= deadline) {
            return fail(ReconnectDiscoveryError{
                .message = cancellation.stop_requested()
                    ? "libusb reconnect discovery was cancelled"
                    : "libusb reconnect discovery deadline expired",
                .native_code = cancellation.stop_requested()
                    ? 0
                    : LIBUSB_ERROR_TIMEOUT,
                .retryable = false,
            });
        }

        std::vector<ReconnectCandidate> candidates;
        candidates.reserve(snapshot->size());
        for (auto& device : *snapshot) {
            ReconnectCandidate candidate{
                .physical_port = UsbPhysicalPortPath{
                    .bus_number = device.bus_number,
                    .ports = device.port_path,
                },
                .serial = device.serial_utf8.empty()
                    ? std::nullopt
                    : std::optional<std::string>{device.serial_utf8},
                .usb_fingerprint = reconnect_fingerprint(device),
                .open_capability = nullptr,
            };
            if (!valid_ports(candidate.physical_port.ports) ||
                !valid_optional_text(candidate.serial) ||
                !valid_fingerprint(candidate.usb_fingerprint)) {
                return fail(ReconnectDiscoveryError{
                    .message =
                        "libusb returned an invalid passive Fastboot candidate",
                    .native_code = 0,
                    .retryable = false,
                });
            }
            candidate.open_capability =
                std::make_shared<CandidateOpenCapability>(
                    owner_identity_, candidate, std::move(device));
            candidates.push_back(std::move(candidate));
        }
        if (cancellation.stop_requested() ||
            ReconnectClock::now() >= deadline) {
            return fail(ReconnectDiscoveryError{
                .message = cancellation.stop_requested()
                    ? "libusb reconnect discovery was cancelled"
                    : "libusb reconnect discovery deadline expired",
                .native_code = cancellation.stop_requested()
                    ? 0
                    : LIBUSB_ERROR_TIMEOUT,
                .retryable = false,
            });
        }
        return candidates;
    } catch (const std::bad_alloc&) {
        return fail(ReconnectDiscoveryError{
            .message = {},
            .native_code = LIBUSB_ERROR_NO_MEM,
            .retryable = false,
        });
    } catch (...) {
        return fail(ReconnectDiscoveryError{
            .message = {},
            .native_code = LIBUSB_ERROR_OTHER,
            .retryable = false,
        });
    }
}

std::expected<OpenedReconnectSession, ReconnectOpenError>
LibusbReconnectAdapter::open(
    const ReconnectCandidate& candidate,
    const ReconnectTimePoint deadline,
    const std::stop_token cancellation) {
    auto attempt_certainty = protocol::TransferCertainty::NotTransferred;
    bool probe_in_flight = false;
    try {
        if (cancellation.stop_requested()) {
            return std::unexpected(ReconnectOpenError{
                .message = "libusb reconnect open was cancelled",
                .native_code = 0,
                .retryable = false,
                .outbound_certainty =
                    protocol::TransferCertainty::NotTransferred,
            });
        }
        if (ReconnectClock::now() >= deadline) {
            return std::unexpected(ReconnectOpenError{
                .message = "libusb reconnect open deadline expired",
                .native_code = LIBUSB_ERROR_TIMEOUT,
                .retryable = false,
                .outbound_certainty =
                    protocol::TransferCertainty::NotTransferred,
            });
        }
        const auto capability = std::dynamic_pointer_cast<
            const CandidateOpenCapability>(candidate.open_capability);
        auto snapshot = capability != nullptr &&
                capability->belongs_to(owner_identity_, candidate)
            ? capability->consume()
            : std::nullopt;
        if (!snapshot.has_value()) {
            return std::unexpected(ReconnectOpenError{
                .message =
                    "libusb reconnect candidate has no valid one-shot open capability",
                .native_code = 0,
                .retryable = false,
                .outbound_certainty =
                    protocol::TransferCertainty::NotTransferred,
            });
        }

        auto verified = runtime_->open_bulk_out_verified(
            *snapshot, deadline, cancellation, options_.transport.bulk_out);
        if (!verified.has_value()) {
            return std::unexpected(open_runtime_error(verified.error()));
        }
        const auto verified_usb = verified->verified_identity();
        const ReconnectCandidate opened_candidate{
            .physical_port = UsbPhysicalPortPath{
                .bus_number = verified_usb.bus_number,
                .ports = verified_usb.port_path,
            },
            .serial = verified_usb.serial_utf8.empty()
                ? std::nullopt
                : std::optional<std::string>{verified_usb.serial_utf8},
            .usb_fingerprint = reconnect_fingerprint(verified_usb),
            .open_capability = nullptr,
        };
        if (!same_candidate(candidate, opened_candidate)) {
            return std::unexpected(ReconnectOpenError{
                .message = "USB identity changed during reconnect open",
                .native_code = 0,
                .retryable = true,
                .outbound_certainty =
                    protocol::TransferCertainty::NotTransferred,
            });
        }

        auto transport_options = options_.transport;
        transport_options.absolute_deadline =
            transport_options.absolute_deadline.has_value()
            ? std::min(*transport_options.absolute_deadline, deadline)
            : deadline;
        auto transport = transport::UsbFastbootTransport::adopt_verified(
            std::move(*verified), std::move(transport_options));
        if (!transport.has_value()) {
            return std::unexpected(open_runtime_error(transport.error()));
        }
        auto session = std::make_unique<protocol::FastbootSession>(
            std::move(*transport), options_.protocol);
        ProbeInterruptionGuard interruption(*session, deadline, cancellation);
        if (const auto interrupted = interrupted_probe(
                interruption,
                deadline,
                cancellation,
                attempt_certainty)) {
            return std::unexpected(*interrupted);
        }

        PrimitiveService service(*session);
        probe_in_flight = true;
        auto product = service.getvar("product");
        probe_in_flight = false;
        if (!product.has_value()) {
            return std::unexpected(probe_error(
                std::move(product.error()),
                attempt_certainty,
                interruption,
                deadline,
                cancellation));
        }
        attempt_certainty = aggregate_certainty(
            attempt_certainty, product->outbound_certainty);
        if (const auto interrupted = interrupted_probe(
                interruption,
                deadline,
                cancellation,
                attempt_certainty)) {
            return std::unexpected(*interrupted);
        }
        if (!valid_text(product->terminal.payload)) {
            return std::unexpected(ReconnectOpenError{
                .message =
                    "Fastboot getvar:product returned an invalid identity",
                .native_code = 0,
                .retryable = false,
                .outbound_certainty = attempt_certainty,
            });
        }

        auto mode = FastbootUsbMode::Bootloader;
        probe_in_flight = true;
        auto userspace = service.getvar("is-userspace");
        probe_in_flight = false;
        if (!userspace.has_value()) {
            attempt_certainty = aggregate_certainty(
                attempt_certainty, userspace.error().outbound_certainty);
            if (userspace.error().code != PrimitiveErrorCode::DeviceFail) {
                return std::unexpected(probe_error(
                    std::move(userspace.error()),
                    attempt_certainty,
                    interruption,
                    deadline,
                    cancellation));
            }
        } else {
            attempt_certainty = aggregate_certainty(
                attempt_certainty, userspace->outbound_certainty);
            if (userspace->terminal.payload == "yes") {
                mode = FastbootUsbMode::Fastbootd;
            } else if (userspace->terminal.payload != "no") {
                return std::unexpected(ReconnectOpenError{
                    .message =
                        "Fastboot getvar:is-userspace must return yes or no",
                    .native_code = 0,
                    .retryable = false,
                    .outbound_certainty = attempt_certainty,
                });
            }
        }
        if (const auto interrupted = interrupted_probe(
                interruption,
                deadline,
                cancellation,
                attempt_certainty)) {
            return std::unexpected(*interrupted);
        }

        auto serial = opened_candidate.serial;
        probe_in_flight = true;
        auto live_serial = service.getvar("serialno");
        probe_in_flight = false;
        if (!live_serial.has_value()) {
            attempt_certainty = aggregate_certainty(
                attempt_certainty,
                live_serial.error().outbound_certainty);
            if (live_serial.error().code != PrimitiveErrorCode::DeviceFail) {
                return std::unexpected(probe_error(
                    std::move(live_serial.error()),
                    attempt_certainty,
                    interruption,
                    deadline,
                    cancellation));
            }
            // FAIL is the protocol-level unsupported result. Retain the
            // descriptor serial, if any; the coordinator still requires it to
            // match the original prepared binding.
        } else {
            attempt_certainty = aggregate_certainty(
                attempt_certainty, live_serial->outbound_certainty);
            if (!valid_text(live_serial->terminal.payload)) {
                return std::unexpected(ReconnectOpenError{
                    .message =
                        "Fastboot getvar:serialno returned an invalid identity",
                    .native_code = 0,
                    .retryable = false,
                    .outbound_certainty = attempt_certainty,
                });
            }
            serial = std::move(live_serial->terminal.payload);
        }
        if (const auto interrupted = interrupted_probe(
                interruption,
                deadline,
                cancellation,
                attempt_certainty)) {
            return std::unexpected(*interrupted);
        }
        if (session->state() != protocol::SessionState::Ready) {
            return std::unexpected(ReconnectOpenError{
                .message =
                    "Fastboot reconnect identity probe did not leave a Ready session",
                .native_code = 0,
                .retryable = false,
                .outbound_certainty = attempt_certainty,
            });
        }
        if (const auto interrupted = finish_probe(
                interruption,
                deadline,
                cancellation,
                attempt_certainty)) {
            return std::unexpected(*interrupted);
        }

        return OpenedReconnectSession{
            .verified_identity = ReconnectDeviceIdentity{
                .physical_port = opened_candidate.physical_port,
                .serial = std::move(serial),
                .usb_fingerprint = opened_candidate.usb_fingerprint,
                .product = std::move(product->terminal.payload),
                .mode = mode,
            },
            .session = std::move(session),
            .outbound_certainty = attempt_certainty,
        };
    } catch (const std::bad_alloc&) {
        const auto certainty = probe_in_flight
            ? protocol::TransferCertainty::PartialOrUnknown
            : attempt_certainty;
        return std::unexpected(ReconnectOpenError{
            .message = {},
            .native_code = LIBUSB_ERROR_NO_MEM,
            .retryable = false,
            .outbound_certainty = certainty,
        });
    } catch (...) {
        const auto certainty = probe_in_flight
            ? protocol::TransferCertainty::PartialOrUnknown
            : attempt_certainty;
        return std::unexpected(ReconnectOpenError{
            .message = {},
            .native_code = LIBUSB_ERROR_OTHER,
            .retryable = false,
            .outbound_certainty = certainty,
        });
    }
}

}  // namespace kairosboot::fastboot
