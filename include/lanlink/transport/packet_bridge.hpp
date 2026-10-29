#pragma once

#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/session/network_state.hpp"
#include "lanlink/network/multicast.hpp"
#include "lanlink/network/lan_discovery.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace lanlink::transport {

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
    void synchronize_multicast(const std::optional<std::vector<std::uint32_t>>& groups);
    void set_mtu(std::uint16_t mtu);

private:
    struct Session {
        protocol::NetworkPeerState peers;
        NetworkKeySnapshot key;
        auth::SignedDeviceKey own_signed_key;
        std::shared_ptr<auth::NetworkPacketCipher> cipher;
        network::MulticastMembership multicast;
        network::LanDiscoveryGuard outbound_discovery;
        std::map<auth::DeviceId, network::LanDiscoveryGuard> inbound_discovery;
    };

    SendHandler send_;
    InjectHandler inject_;
    std::mutex mutex_;
    std::map<protocol::NetworkId, std::shared_ptr<Session>> sessions_;
    std::uint16_t mtu_ = 65535;
};

}
