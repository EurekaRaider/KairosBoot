// SPDX-License-Identifier: MIT
#pragma once

#include "src/protocol/transport_session.hpp"

#include <memory>

namespace kairosboot::transport {

// Adds bounded sequential ITransferSource support to stream transports such as
// Fastboot TCP/UDP. USB keeps its specialized asynchronous transfer ring.
[[nodiscard]] std::unique_ptr<protocol::ITransportSession>
make_sequential_streaming_transport(
    std::unique_ptr<protocol::ITransportSession> transport);

}  // namespace kairosboot::transport
