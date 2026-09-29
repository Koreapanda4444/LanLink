#pragma once

#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/transport/quic.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace lanlink::transport {

struct ActiveNetworkPacketState {
    protocol::NetworkPeerState peers;
    NetworkKeySnapshot key;
};

class VirtualPacketBridge {
public:
    using SendHandler = std::function<void(const protocol::EncryptedNetworkPacket&)>;
    using InjectHandler = std::function<void(std::span<const std::byte>)>;

    VirtualPacketBridge(SendHandler send, InjectHandler inject);

    void synchronize(const std::optional<PacketIdentitySnapshot>& identity,
                     std::span<const ActiveNetworkPacketState> networks);
    void outbound(std::span<const std::byte> ipv4_packet);
    void inbound(const protocol::ForwardedNetworkPacket& delivery);
    void clear() noexcept;

private:
    struct Session {
        protocol::NetworkPeerState peers;
        NetworkKeySnapshot key;
        auth::SignedDeviceKey own_signed_key;
        std::shared_ptr<auth::NetworkPacketCipher> cipher;
    };

    SendHandler send_;
    InjectHandler inject_;
    std::mutex mutex_;
    std::map<protocol::NetworkId, std::shared_ptr<Session>> sessions_;
};

}
