#pragma once

#include "lanlink/protocol/packet_messages.hpp"
#include "lanlink/protocol/path_diagnostics.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace lanlink::transport {

using protocol::PathKind;
using protocol::PathLifecycle;
using protocol::PathReason;
using protocol::PathStatus;
enum class PacketSendResult : std::uint8_t { accepted, unavailable, backpressure, invalid_packet };

class PacketPath {
public:
    using ReceiveHandler = std::function<void(const protocol::ForwardedNetworkPacket&)>;
    virtual ~PacketPath() = default;
    virtual void start() = 0;
    virtual void stop() noexcept = 0;
    virtual void set_receive_handler(ReceiveHandler handler) = 0;
    [[nodiscard]] virtual PacketSendResult send(const protocol::EncryptedNetworkPacket& packet) = 0;
    [[nodiscard]] virtual PathStatus status() const = 0;
};

}
