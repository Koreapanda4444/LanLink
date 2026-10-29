#pragma once

#include "lanlink/network/virtual_adapter.hpp"
#include <array>

namespace lanlink::network {

inline constexpr std::uint16_t ethernet_mtu = 1280;
inline constexpr std::size_t ethernet_tunnel_header = 32;
using MacAddress = std::array<std::byte, 6>;
[[nodiscard]] MacAddress virtual_mac(std::uint32_t address) noexcept;
[[nodiscard]] std::vector<std::byte> encapsulate_ethernet(
    std::span<const std::byte> frame, const VirtualNetworkAddress& network);
[[nodiscard]] std::vector<std::byte> decapsulate_ethernet(
    std::span<const std::byte> packet, const VirtualNetworkAddress& network);

}
