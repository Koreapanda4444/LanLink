#include "lanlink/protocol/ipv4.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lanlink::protocol {
namespace {
std::uint32_t get(std::span<const std::byte> bytes, std::size_t offset, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index) value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + index]);
    return value;
}
void put(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        bytes[offset + index] = std::byte{static_cast<unsigned char>(value >> ((count - index - 1) * 8U))};
}
std::vector<std::byte> copied_options(std::span<const std::byte> packet, std::size_t header) {
    std::vector<std::byte> result;
    for (std::size_t offset = 20; offset < header;) {
        const auto type = std::to_integer<unsigned char>(packet[offset]);
        if (type == 0) break;
        if (type == 1) { ++offset; continue; }
        if (offset + 2 > header) throw std::invalid_argument("truncated IPv4 option");
        const auto size = std::to_integer<unsigned char>(packet[offset + 1]);
        if (size < 2 || size > header - offset) throw std::invalid_argument("invalid IPv4 option length");
        if (type & 0x80U) result.insert(result.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset), packet.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    while (result.size() % 4) result.push_back(std::byte{0});
    return result;
}
}

std::uint16_t internet_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t index = 0; index < bytes.size(); index += 2) {
        sum += std::to_integer<unsigned char>(bytes[index]) << 8U;
        if (index + 1 < bytes.size()) sum += std::to_integer<unsigned char>(bytes[index + 1]);
    }
    while (sum >> 16U) sum = (sum & 0xffffU) + (sum >> 16U);
    return static_cast<std::uint16_t>(~sum);
}
void update_ipv4_checksum(std::span<std::byte> packet) {
    if (packet.size() < 20) throw std::invalid_argument("short IPv4 header");
    const auto header = (std::to_integer<unsigned char>(packet[0]) & 15U) * 4U;
    if (header < 20 || header > packet.size()) throw std::invalid_argument("invalid IPv4 header length");
    packet[10] = packet[11] = std::byte{0};
    put(packet, 10, internet_checksum(packet.first(header)), 2);
}
Ipv4Header inspect_ipv4(std::span<const std::byte> packet) {
    if (packet.size() < 20 || packet.size() > 65535 || (std::to_integer<unsigned char>(packet[0]) >> 4U) != 4)
        throw std::invalid_argument("invalid IPv4 packet size or version");
    const auto header = (std::to_integer<unsigned char>(packet[0]) & 15U) * 4U;
    if (header < 20 || header > packet.size() || get(packet, 2, 2) != packet.size() ||
        packet[8] == std::byte{0} || internet_checksum(packet.first(header)) != 0)
        throw std::invalid_argument("invalid IPv4 header or checksum");
    static_cast<void>(copied_options(packet, header));
    const auto flags = get(packet, 6, 2);
    const auto offset = (flags & 0x1fffU) * 8U;
    const bool more = (flags & 0x2000U) != 0, df = (flags & 0x4000U) != 0;
    const auto payload = packet.size() - header;
    if ((flags & 0x8000U) || (df && (more || offset)) || ((more || offset) && payload == 0) ||
        (more && payload % 8U) || offset + payload > 65535U - header)
        throw std::invalid_argument("invalid IPv4 fragment bounds");
    return {header, get(packet, 12, 4), get(packet, 16, 4), static_cast<std::uint16_t>(offset), more, df};
}

std::vector<std::vector<std::byte>> fragment_ipv4(std::span<const std::byte> packet, const std::uint16_t mtu) {
    if (mtu < 68) throw std::invalid_argument("IPv4 MTU is below 68");
    const auto info = inspect_ipv4(packet);
    if (packet.size() <= mtu) return {{packet.begin(), packet.end()}};
    if (info.dont_fragment) throw std::length_error("IPv4 fragmentation needed");
    const auto options = copied_options(packet, info.header_size);
    std::vector<std::vector<std::byte>> result;
    for (std::size_t offset = 0; offset < packet.size() - info.header_size;) {
        const auto header = offset == 0 ? info.header_size : 20 + options.size();
        const auto remaining = packet.size() - info.header_size - offset;
        const auto count = remaining <= mtu - header ? remaining : ((mtu - header) / 8U) * 8U;
        if (count == 0) throw std::invalid_argument("MTU cannot hold an IPv4 fragment");
        std::vector<std::byte> fragment(packet.begin(), packet.begin() + 20);
        if (offset == 0) fragment.insert(fragment.end(), packet.begin() + 20, packet.begin() + static_cast<std::ptrdiff_t>(info.header_size));
        else fragment.insert(fragment.end(), options.begin(), options.end());
        fragment.insert(fragment.end(), packet.begin() + static_cast<std::ptrdiff_t>(info.header_size + offset),
                        packet.begin() + static_cast<std::ptrdiff_t>(info.header_size + offset + count));
        fragment[0] = std::byte{static_cast<unsigned char>(0x40U | (header / 4U))};
        put(fragment, 2, static_cast<std::uint32_t>(fragment.size()), 2);
        put(fragment, 6, ((info.fragment_offset + offset) / 8U) |
            ((count < remaining || info.more_fragments) ? 0x2000U : 0), 2);
        update_ipv4_checksum(fragment); result.push_back(std::move(fragment)); offset += count;
    }
    return result;
}

std::optional<std::vector<std::byte>> ipv4_fragmentation_needed(std::span<const std::byte> original,
    const std::uint32_t router, const std::uint16_t mtu) {
    const auto info = inspect_ipv4(original);
    if (!info.dont_fragment || info.fragment_offset || info.source == 0 || info.source >= 0xe0000000U ||
        info.destination >= 0xe0000000U || router == 0 || router >= 0xe0000000U) return std::nullopt;
    if (original[9] == std::byte{1} && original.size() > info.header_size) {
        const auto type = std::to_integer<unsigned char>(original[info.header_size]);
        if (type == 3 || type == 4 || type == 5 || type == 11 || type == 12) return std::nullopt;
    }
    const auto quote = std::min(original.size(), info.header_size + 8);
    std::vector<std::byte> packet(28);
    packet[0] = std::byte{0x45}; packet[8] = std::byte{64}; packet[9] = std::byte{1};
    put(packet, 12, router, 4); put(packet, 16, info.source, 4);
    packet[20] = std::byte{3}; packet[21] = std::byte{4}; put(packet, 26, mtu, 2);
    packet.insert(packet.end(), original.begin(), original.begin() + static_cast<std::ptrdiff_t>(quote));
    put(packet, 2, static_cast<std::uint32_t>(packet.size()), 2);
    put(packet, 22, internet_checksum(std::span<const std::byte>(packet).subspan(20)), 2);
    update_ipv4_checksum(packet);
    return packet;
}
}
