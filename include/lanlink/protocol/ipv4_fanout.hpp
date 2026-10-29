#pragma once

#include <cstdint>

namespace lanlink::protocol {

[[nodiscard]] inline bool is_ipv4_subnet(const std::uint32_t subnet,
                                         const std::uint8_t prefix_length) noexcept {
    return prefix_length > 0 && prefix_length <= 30 &&
           (subnet & ~(0xffffffffU << (32U - prefix_length))) == 0;
}

[[nodiscard]] inline bool is_ipv4_host(const std::uint32_t address,
                                      const std::uint32_t subnet,
                                      const std::uint8_t prefix_length) noexcept {
    if (!is_ipv4_subnet(subnet, prefix_length)) return false;
    const auto mask = 0xffffffffU << (32U - prefix_length);
    return address != subnet && address != (subnet | ~mask) &&
           address < 0xe0000000U && (address & mask) == subnet;
}

[[nodiscard]] inline bool is_ipv4_multicast(const std::uint32_t address) noexcept {
    return (address & 0xf0000000U) == 0xe0000000U;
}

[[nodiscard]] inline bool is_ipv4_fanout_destination(
    const std::uint32_t destination, const std::uint32_t subnet,
    const std::uint8_t prefix_length) noexcept {
    if (!is_ipv4_subnet(subnet, prefix_length)) {
        return false;
    }
    const auto mask = 0xffffffffU << (32U - prefix_length);
    return destination == 0xffffffffU || is_ipv4_multicast(destination) ||
           destination == (subnet | ~mask);
}

}
