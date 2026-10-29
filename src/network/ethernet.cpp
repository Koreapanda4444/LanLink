#include "lanlink/network/ethernet.hpp"
#include "lanlink/protocol/ipv4.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"

#include <algorithm>
#include <stdexcept>

namespace lanlink::network {
namespace {
constexpr std::uint16_t tunnel_port = 48042;

std::uint32_t get(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + i]);
    return value;
}
void put(std::span<std::byte> bytes, std::size_t offset, std::size_t size, std::uint32_t value) {
    for (std::size_t i = 0; i < size; ++i)
        bytes[offset + i] = std::byte{static_cast<unsigned char>(value >> ((size - i - 1) * 8U))};
}
bool same_mac(std::span<const std::byte> frame, std::size_t offset, const MacAddress& mac) {
    return std::equal(mac.begin(), mac.end(), frame.begin() + static_cast<std::ptrdiff_t>(offset));
}
void validate_network(const VirtualNetworkAddress& network) {
    if (!protocol::is_ipv4_host(network.address, network.subnet, network.prefix_length))
        throw std::invalid_argument("invalid TAP network address");
}
std::uint32_t validate_frame(std::span<const std::byte> frame,
    const VirtualNetworkAddress& network, std::uint32_t sender) {
    if (frame.size() < 14 || frame.size() > ethernet_mtu + 14U ||
        !protocol::is_ipv4_host(sender, network.subnet, network.prefix_length) ||
        !same_mac(frame, 6, virtual_mac(sender)))
        throw std::invalid_argument("invalid Ethernet size or source");
    const bool broadcast = std::all_of(frame.begin(), frame.begin() + 6,
        [](std::byte value) { return value == std::byte{255}; });
    const auto destination = get(frame, 2, 4);
    const auto type = get(frame, 12, 2);
    std::uint32_t target = 0;
    if (type == 0x0806) {
        if (frame.size() < 42 || get(frame, 14, 2) != 1 || get(frame, 16, 2) != 0x0800 ||
            frame[18] != std::byte{6} || frame[19] != std::byte{4} ||
            (get(frame, 20, 2) != 1 && get(frame, 20, 2) != 2) ||
            !same_mac(frame, 22, virtual_mac(sender)) || get(frame, 28, 4) != sender)
            throw std::invalid_argument("invalid ARP identity");
        target = get(frame, 38, 4);
        if (!protocol::is_ipv4_host(target, network.subnet, network.prefix_length))
            throw std::invalid_argument("ARP target is outside the virtual subnet");
    } else if (type == 0x0800) {
        if (frame.size() < 34) throw std::invalid_argument("truncated Ethernet IPv4 packet");
        const auto length = get(frame, 16, 2);
        if (length < 20 || length > frame.size() - 14)
            throw std::invalid_argument("invalid Ethernet IPv4 length");
        const auto ip = frame.subspan(14, length);
        static_cast<void>(protocol::inspect_ipv4(ip));
        if (get(ip, 12, 4) != sender)
            throw std::invalid_argument("Ethernet IPv4 source does not match membership");
        target = get(ip, 16, 4);
        if (!protocol::is_ipv4_host(target, network.subnet, network.prefix_length) &&
            !protocol::is_ipv4_fanout_destination(target, network.subnet, network.prefix_length))
            throw std::invalid_argument("Ethernet IPv4 destination is outside the virtual subnet");
    } else {
        throw std::invalid_argument("TAP supports ARP and scoped IPv4 Ethernet only");
    }
    if (broadcast) return 0xffffffffU;
    if (type == 0x0800 && protocol::is_ipv4_scoped_multicast(target)) {
        const MacAddress multicast{std::byte{1}, std::byte{0}, std::byte{0x5e},
            std::byte{static_cast<unsigned char>((target >> 16U) & 0x7fU)},
            std::byte{static_cast<unsigned char>(target >> 8U)},
            std::byte{static_cast<unsigned char>(target)}};
        if (same_mac(frame, 0, multicast)) return 0xffffffffU;
    }
    if (!protocol::is_ipv4_host(destination, network.subnet, network.prefix_length) ||
        !same_mac(frame, 0, virtual_mac(destination)) || destination != target)
        throw std::invalid_argument("Ethernet destination does not match the virtual address");
    return destination;
}
}

MacAddress virtual_mac(const std::uint32_t address) noexcept {
    return {std::byte{2}, std::byte{0x4c},
        std::byte{static_cast<unsigned char>(address >> 24U)},
        std::byte{static_cast<unsigned char>(address >> 16U)},
        std::byte{static_cast<unsigned char>(address >> 8U)},
        std::byte{static_cast<unsigned char>(address)}};
}

std::vector<std::byte> encapsulate_ethernet(std::span<const std::byte> frame,
    const VirtualNetworkAddress& network) {
    validate_network(network);
    const auto target = validate_frame(frame, network, network.address);
    std::vector<std::byte> packet(ethernet_tunnel_header + frame.size());
    packet[0] = std::byte{0x45}; put(packet, 2, 2, static_cast<std::uint32_t>(packet.size()));
    packet[6] = std::byte{0x40}; packet[8] = std::byte{64}; packet[9] = std::byte{17};
    put(packet, 12, 4, network.address); put(packet, 16, 4, target);
    put(packet, 20, 2, tunnel_port); put(packet, 22, 2, tunnel_port);
    put(packet, 24, 2, static_cast<std::uint32_t>(packet.size() - 20));
    packet[28] = std::byte{'L'}; packet[29] = std::byte{'L'};
    packet[30] = std::byte{'2'}; packet[31] = std::byte{1};
    std::copy(frame.begin(), frame.end(), packet.begin() + ethernet_tunnel_header);
    protocol::update_ipv4_checksum(packet);
    return packet;
}

std::vector<std::byte> decapsulate_ethernet(std::span<const std::byte> packet,
    const VirtualNetworkAddress& network) {
    validate_network(network);
    if (packet.size() < ethernet_tunnel_header + 14 || packet.size() > ethernet_tunnel_header + ethernet_mtu + 14U ||
        packet[0] != std::byte{0x45} || packet[9] != std::byte{17} ||
        (get(packet, 6, 2) & 0x3fffU) != 0 || get(packet, 20, 2) != tunnel_port ||
        get(packet, 22, 2) != tunnel_port || get(packet, 24, 2) != packet.size() - 20 ||
        get(packet, 26, 2) != 0 || packet[28] != std::byte{'L'} || packet[29] != std::byte{'L'} ||
        packet[30] != std::byte{'2'} || packet[31] != std::byte{1})
        throw std::invalid_argument("invalid Ethernet tunnel packet");
    static_cast<void>(protocol::inspect_ipv4(packet));
    const auto frame = packet.subspan(ethernet_tunnel_header);
    const auto target = validate_frame(frame, network, get(packet, 12, 4));
    if (get(packet, 16, 4) != target || (target != 0xffffffffU && target != network.address))
        throw std::invalid_argument("Ethernet tunnel destination mismatch");
    return {frame.begin(), frame.end()};
}

}
