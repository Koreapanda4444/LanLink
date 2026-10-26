#pragma once

#include "lanlink/protocol/packet_messages.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace lanlink::transport {

enum class PathKind : std::uint8_t { none, relay_quic, relay_tls, direct_quic };
enum class PathLifecycle : std::uint8_t { stopped, connecting, authenticated, failed };
enum class PacketSendResult : std::uint8_t { accepted, unavailable, backpressure, invalid_packet };

struct PathStatus {
    PathKind kind = PathKind::none;
    PathLifecycle lifecycle = PathLifecycle::stopped;
    std::optional<std::uint32_t> rtt_us;
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
};

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
