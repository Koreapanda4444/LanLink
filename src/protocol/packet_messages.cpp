#include "lanlink/protocol/packet_messages.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lanlink::protocol {
namespace {

constexpr std::size_t header_size = network_id_size + 4 + 8 + 4 + packet_nonce_size + 2;
constexpr std::size_t fixed_size = header_size + packet_tag_size;

template <typename Integer>
void append_integer(std::vector<std::byte>& output, const Integer value) {
    for (std::size_t index = sizeof(Integer); index > 0; --index) {
        output.push_back(std::byte{static_cast<unsigned char>(
            value >> ((index - 1) * 8U))});
    }
}

template <typename Integer>
Integer read_integer(const std::span<const std::byte> payload,
                     const std::size_t offset) {
    if (offset > payload.size() || sizeof(Integer) > payload.size() - offset) {
        throw std::runtime_error("encrypted packet is truncated");
    }
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value = static_cast<Integer>((value << 8U) |
            std::to_integer<unsigned char>(payload[offset + index]));
    }
    return value;
}

bool valid_packet(const EncryptedNetworkPacket& packet) {
    const auto has_network = std::any_of(packet.network_id.begin(),
                                         packet.network_id.end(),
        [](const std::byte byte) { return byte != std::byte{0}; });
    if (!has_network || packet.destination_ipv4 == 0 ||
        (packet.destination_ipv4 >= 0xf0000000U &&
         packet.destination_ipv4 != 0xffffffffU) || packet.key_epoch == 0 ||
        packet.sequence == 0 ||
        packet.ciphertext.size() < min_virtual_ipv4_packet_size ||
        packet.ciphertext.size() > max_virtual_ipv4_packet_size) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (packet.nonce[8 + index] !=
            std::byte{static_cast<unsigned char>(
                packet.sequence >> ((3 - index) * 8U))}) {
            return false;
        }
    }
    return true;
}

}

std::vector<std::byte> encode_encrypted_network_packet(
    const EncryptedNetworkPacket& packet) {
    if (!valid_packet(packet)) {
        throw std::invalid_argument("encrypted packet header or length is invalid");
    }
    std::vector<std::byte> output;
    output.reserve(fixed_size + packet.ciphertext.size());
    output.insert(output.end(), packet.network_id.begin(), packet.network_id.end());
    append_integer(output, packet.destination_ipv4);
    append_integer(output, packet.key_epoch);
    append_integer(output, packet.sequence);
    output.insert(output.end(), packet.nonce.begin(), packet.nonce.end());
    append_integer(output, static_cast<std::uint16_t>(packet.ciphertext.size()));
    output.insert(output.end(), packet.ciphertext.begin(), packet.ciphertext.end());
    output.insert(output.end(), packet.tag.begin(), packet.tag.end());
    return output;
}

EncryptedNetworkPacket decode_encrypted_network_packet(
    const std::span<const std::byte> payload) {
    if (payload.size() < fixed_size + min_virtual_ipv4_packet_size ||
        payload.size() > fixed_size + max_virtual_ipv4_packet_size) {
        throw std::runtime_error("encrypted packet size is invalid");
    }
    const auto size = read_integer<std::uint16_t>(payload, header_size - 2);
    if (payload.size() != fixed_size + size) {
        throw std::runtime_error("encrypted packet length does not match payload");
    }

    EncryptedNetworkPacket packet;
    std::copy_n(payload.begin(), network_id_size, packet.network_id.begin());
    packet.destination_ipv4 = read_integer<std::uint32_t>(payload, network_id_size);
    packet.key_epoch = read_integer<std::uint64_t>(payload, network_id_size + 4);
    packet.sequence = read_integer<std::uint32_t>(payload, network_id_size + 12);
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(network_id_size + 16),
                packet_nonce_size, packet.nonce.begin());
    packet.ciphertext.assign(payload.begin() + static_cast<std::ptrdiff_t>(header_size),
                             payload.begin() + static_cast<std::ptrdiff_t>(header_size + size));
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(header_size + size),
                packet_tag_size, packet.tag.begin());
    if (!valid_packet(packet)) {
        throw std::runtime_error("encrypted packet header or length is invalid");
    }
    return packet;
}

std::vector<std::byte> encode_forwarded_network_packet(
    const ForwardedNetworkPacket& packet) {
    if (std::all_of(packet.sender_device_id.begin(), packet.sender_device_id.end(),
                    [](const std::byte value) { return value == std::byte{0}; })) {
        throw std::invalid_argument("forwarded packet sender is empty");
    }
    auto encrypted = encode_encrypted_network_packet(packet.packet);
    std::vector<std::byte> output;
    output.reserve(packet.sender_device_id.size() + encrypted.size());
    output.insert(output.end(), packet.sender_device_id.begin(),
                  packet.sender_device_id.end());
    output.insert(output.end(), encrypted.begin(), encrypted.end());
    return output;
}

ForwardedNetworkPacket decode_forwarded_network_packet(
    const std::span<const std::byte> payload) {
    if (payload.size() < auth::device_id_size + fixed_size +
                             min_virtual_ipv4_packet_size) {
        throw std::runtime_error("forwarded packet is truncated");
    }
    ForwardedNetworkPacket result;
    std::copy_n(payload.begin(), result.sender_device_id.size(),
                result.sender_device_id.begin());
    if (std::all_of(result.sender_device_id.begin(), result.sender_device_id.end(),
                    [](const std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("forwarded packet sender is empty");
    }
    result.packet = decode_encrypted_network_packet(payload.subspan(
        result.sender_device_id.size()));
    return result;
}

}
