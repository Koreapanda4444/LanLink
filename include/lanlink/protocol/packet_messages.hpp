#pragma once

#include "lanlink/protocol/network_messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lanlink::protocol {

inline constexpr std::size_t min_virtual_ipv4_packet_size = 20;
inline constexpr std::size_t max_virtual_ipv4_packet_size = 65'535;
inline constexpr std::size_t packet_nonce_size = 12;
inline constexpr std::size_t packet_tag_size = 16;

struct EncryptedNetworkPacket {
    NetworkId network_id{};
    std::uint32_t destination_ipv4 = 0;
    std::uint64_t key_epoch = 0;
    std::uint32_t sequence = 0;
    std::array<std::byte, packet_nonce_size> nonce{};
    std::vector<std::byte> ciphertext;
    std::array<std::byte, packet_tag_size> tag{};

    bool operator==(const EncryptedNetworkPacket&) const = default;
};

struct ForwardedNetworkPacket {
    auth::DeviceId sender_device_id{};
    EncryptedNetworkPacket packet;

    bool operator==(const ForwardedNetworkPacket&) const = default;
};

[[nodiscard]] std::vector<std::byte> encode_encrypted_network_packet(
    const EncryptedNetworkPacket& packet);
[[nodiscard]] EncryptedNetworkPacket decode_encrypted_network_packet(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_forwarded_network_packet(
    const ForwardedNetworkPacket& packet);
[[nodiscard]] ForwardedNetworkPacket decode_forwarded_network_packet(
    std::span<const std::byte> payload);

}
