#pragma once
#include "lanlink/p2p/candidates.hpp"
#include <string>

namespace lanlink::p2p {
struct InterfaceAddress {
    std::string name;
    std::uint32_t ipv4 = 0;
    std::uint32_t index = 0;
    bool up = false, loopback = false, tunnel = false;
};
struct ExcludedSubnet { std::uint32_t address = 0; std::uint8_t prefix = 24; };
[[nodiscard]] std::vector<InterfaceAddress> interface_addresses();
[[nodiscard]] std::vector<Candidate> gather_local_candidates(std::span<const InterfaceAddress> interfaces,
    std::uint16_t port, std::span<const ExcludedSubnet> excluded = {}, bool allow_loopback = false);
}
