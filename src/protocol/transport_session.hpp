// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace kairosboot::protocol {

enum class TransportStatus : std::uint8_t {
    Ok,
    Timeout,
    Disconnected,
    IoError,
};

// Describes what the transport can prove about a failed write. Fastboot cannot
// safely retry a command or data chunk when the amount accepted by the device is
// unknown.
enum class TransferCertainty : std::uint8_t {
    NotTransferred,
    FullyTransferred,
    PartialOrUnknown,
};

struct TransferResult {
    TransportStatus status{TransportStatus::Ok};
    std::size_t transferred{0};
    TransferCertainty certainty{TransferCertainty::FullyTransferred};
    bool truncated{false};
    std::string detail;
};

// Internal transport seam shared by USB, TCP and UDP adapters.
//
// write() has stream semantics: a successful short write may be continued with
// the remaining bytes. read() returns exactly one logical framed Fastboot
// response and must set truncated when that response did not fit in the supplied
// buffer. USB adapters consume/ignore transfer-level ZLPs instead of surfacing
// them here; a successful zero-byte logical response is therefore malformed.
class ITransportSession {
public:
    virtual ~ITransportSession() = default;

    [[nodiscard]] virtual TransferResult write(
        std::span<const std::byte> bytes,
        std::chrono::milliseconds timeout) = 0;

    [[nodiscard]] virtual TransferResult read(
        std::span<std::byte> destination,
        std::chrono::milliseconds timeout) = 0;

    virtual void close() noexcept = 0;
};

}  // namespace kairosboot::protocol
