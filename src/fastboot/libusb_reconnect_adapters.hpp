// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/reconnect_coordinator.hpp"
#include "src/fleet/device_preflight.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::fastboot {

enum class PreparedReconnectBindingErrorCode : std::uint8_t {
    InvalidPreparedIdentity,
    InvalidRequiredMode,
    UnsafeSessionState,
    UnsafeTransferOutcome,
    ResourceExhausted,
    UnexpectedFailure,
};

struct PreparedReconnectBindingError final {
    PreparedReconnectBindingErrorCode code{
        PreparedReconnectBindingErrorCode::UnexpectedFailure};
    std::string message;
};

// Move-only identity capability whose constructor is reachable only through a
// PreparedDeviceSession. PreparedDeviceSession itself is created only by the
// complete live preflight/product barrier, so later actors cannot forge an
// initial reconnect identity from manifest or passive USB text alone.
class PreparedReconnectBinding final {
public:
    PreparedReconnectBinding(const PreparedReconnectBinding&) = delete;
    PreparedReconnectBinding& operator=(const PreparedReconnectBinding&) = delete;
    PreparedReconnectBinding(PreparedReconnectBinding&&) noexcept = default;
    PreparedReconnectBinding& operator=(PreparedReconnectBinding&&) noexcept =
        default;
    ~PreparedReconnectBinding() = default;

    [[nodiscard]] std::expected<ReconnectTarget,
                                PreparedReconnectBindingError>
    target_after_transition(
        FastbootUsbMode required_mode,
        protocol::SessionState preceding_session_state,
        protocol::TransferCertainty preceding_operation_certainty) const;

private:
    explicit PreparedReconnectBinding(ReconnectTarget identity) noexcept;

    ReconnectTarget identity_;

    friend std::expected<PreparedReconnectBinding,
                         PreparedReconnectBindingError>
    make_prepared_reconnect_binding(
        const fleet::PreparedDeviceSession&) noexcept;
};

[[nodiscard]] std::expected<PreparedReconnectBinding,
                            PreparedReconnectBindingError>
make_prepared_reconnect_binding(
    const fleet::PreparedDeviceSession& prepared) noexcept;

enum class LibusbReconnectAdapterFactoryErrorCode : std::uint8_t {
    InvalidArgument,
    ResourceExhausted,
    UnexpectedFailure,
};

struct LibusbReconnectAdapterFactoryError final {
    LibusbReconnectAdapterFactoryErrorCode code{
        LibusbReconnectAdapterFactoryErrorCode::UnexpectedFailure};
    std::string message;
};

struct LibusbReconnectAdapterOptions final {
    transport::UsbFastbootTransportOptions transport;
    protocol::SessionOptions protocol;
};

// Production discovery/opener pair. Each passive result carries an
// attempt-local, one-shot open capability for its complete libusb snapshot.
// open() consumes only a capability minted by this adapter, uses verified-open
// exactly once, adopts that claimed interface, and probes the live protocol
// identity before it can publish a Ready session.
class LibusbReconnectAdapter final : public IReconnectDiscovery,
                                      public IReconnectSessionOpener {
public:
    [[nodiscard]] static std::expected<
        std::unique_ptr<LibusbReconnectAdapter>,
        LibusbReconnectAdapterFactoryError>
    create(std::shared_ptr<transport::LibusbRuntime> runtime,
           LibusbReconnectAdapterOptions options = {}) noexcept;

    LibusbReconnectAdapter(const LibusbReconnectAdapter&) = delete;
    LibusbReconnectAdapter& operator=(const LibusbReconnectAdapter&) = delete;
    ~LibusbReconnectAdapter() override = default;

    [[nodiscard]] std::expected<std::vector<ReconnectCandidate>,
                                ReconnectDiscoveryError>
    discover(ReconnectTimePoint deadline,
             std::stop_token cancellation) override;

    [[nodiscard]] std::expected<OpenedReconnectSession, ReconnectOpenError>
    open(const ReconnectCandidate& candidate,
         ReconnectTimePoint deadline,
         std::stop_token cancellation) override;

private:
    class OwnerIdentity;
    class CandidateOpenCapability;

    LibusbReconnectAdapter(
        std::shared_ptr<transport::LibusbRuntime> runtime,
        LibusbReconnectAdapterOptions options,
        std::shared_ptr<const OwnerIdentity> owner_identity) noexcept;

    std::shared_ptr<transport::LibusbRuntime> runtime_;
    LibusbReconnectAdapterOptions options_;
    std::shared_ptr<const OwnerIdentity> owner_identity_;
};

}  // namespace kairosboot::fastboot
