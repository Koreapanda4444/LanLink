#pragma once

#include "lanlink/auth/network_keys.hpp"
#include "lanlink/protocol/packet_messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace lanlink::auth {

class NetworkPacketCipher {
public:
    NetworkPacketCipher(protocol::NetworkId network_id,
                        std::uint64_t key_epoch,
                        NetworkKey network_key,
                        DeviceId own_device_id,
                        const SignedDeviceKey& own_signed_key,
                        std::uint32_t own_ipv4_address);

    NetworkPacketCipher(const NetworkPacketCipher&) = delete;
    NetworkPacketCipher& operator=(const NetworkPacketCipher&) = delete;

    void update_peers(const protocol::NetworkPeerState& state);
    [[nodiscard]] protocol::EncryptedNetworkPacket encrypt(
        std::span<const std::byte> ipv4_packet);
    [[nodiscard]] std::vector<std::byte> decrypt(
        const protocol::EncryptedNetworkPacket& packet,
        const DeviceId& authenticated_sender);

private:
    struct PeerReceiver {
        EncryptionPublicKey session_public_key{};
        std::uint32_t ipv4_address = 0;
        Secret32 encryption_key;
        std::array<std::byte, 8> nonce_prefix{};
        std::uint32_t highest_sequence = 0;
        std::uint64_t seen_sequences = 0;
        bool initialized = false;
    };

    protocol::NetworkId network_id_;
    std::uint64_t key_epoch_;
    NetworkKey network_key_;
    DeviceId own_device_id_;
    EncryptionPublicKey own_session_public_key_;
    std::uint32_t own_ipv4_address_;
    Secret32 outgoing_key_;
    std::array<std::byte, 8> outgoing_nonce_prefix_{};
    std::uint32_t next_sequence_ = 0;
    std::map<DeviceId, PeerReceiver> peers_;
    std::mutex mutex_;
};

}
