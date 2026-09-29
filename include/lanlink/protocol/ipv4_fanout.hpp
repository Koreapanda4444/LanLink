#pragma once

#include <cstdint>

namespace lanlink::protocol {

[[nodiscard]] inline bool is_ipv4_multicast(const std::uint32_t address) noexcept {
    return (address & 0xf0000000U) == 0xe0000000U;
}

[[nodiscard]] inline bool is_ipv4_fanout_destination(
    const std::uint32_t destination, const std::uint32_t subnet,
    const std::uint8_t prefix_length) noexcept {
    if (prefix_length == 0 || prefix_length > 30) {
        return false;
    }
    const auto mask = 0xffffffffU << (32U - prefix_length);
    return destination == 0xffffffffU || is_ipv4_multicast(destination) ||
           destination == (subnet | ~mask);
}

}
