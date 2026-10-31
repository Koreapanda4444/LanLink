#pragma once
#include "lanlink/session/network_state.hpp"
namespace lanlink::network {
struct AdapterRoute {
    protocol::NetworkId network_id{};
    std::uint32_t address=0;
    std::uint8_t prefix=24;
    bool operator==(const AdapterRoute&) const = default;
};
struct AdapterForward {
    protocol::NetworkId network_id{};
    std::uint32_t subnet=0;
    std::uint8_t prefix=24;
    std::uint32_t member_address=0;
    bool operator==(const AdapterForward&) const = default;
};
struct RoutingConfiguration {
    std::vector<AdapterRoute> routes;
    std::vector<AdapterForward> forwards;
    bool operator==(const RoutingConfiguration&) const = default;
};
[[nodiscard]] RoutingConfiguration routing_configuration(const session::NetworkSnapshot& snapshot,bool accept_routes,bool forward);
[[nodiscard]] bool safe_routed_ipv4(std::span<const std::byte> packet);
[[nodiscard]] std::vector<std::byte> encapsulate_routed_ipv4(std::span<const std::byte> inner,std::uint32_t source,std::uint32_t gateway);
[[nodiscard]] std::span<const std::byte> decapsulate_routed_ipv4(std::span<const std::byte> outer);
}
