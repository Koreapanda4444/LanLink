#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lanlink::protocol {
struct Ipv4Header {
    std::size_t header_size = 0;
    std::uint32_t source = 0, destination = 0;
    std::uint16_t fragment_offset = 0;
    bool more_fragments = false, dont_fragment = false;
};
[[nodiscard]] std::uint16_t internet_checksum(std::span<const std::byte> bytes) noexcept;
void update_ipv4_checksum(std::span<std::byte> packet);
[[nodiscard]] Ipv4Header inspect_ipv4(std::span<const std::byte> packet);
[[nodiscard]] std::vector<std::vector<std::byte>> fragment_ipv4(std::span<const std::byte> packet, std::uint16_t mtu);
[[nodiscard]] std::optional<std::vector<std::byte>> ipv4_fragmentation_needed(
    std::span<const std::byte> original, std::uint32_t router, std::uint16_t mtu);
}
