#pragma once
#include "lanlink/auth/device_keys.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanlink::protocol {
enum class NetworkTopology : std::uint8_t { mesh = 0, hub_spoke = 1 };
enum class AccessRole : std::uint8_t { member = 0, administrator = 1, restricted = 2 };
struct PolicySubject {
    auth::DeviceId device{};
    std::string group; // Empty device and group means all members.
    bool operator==(const PolicySubject&) const = default;
};
struct AccessRule {
    PolicySubject first, second;
    bool allow = false; // Deny wins, otherwise a matching allow is required.
    bool operator==(const AccessRule&) const = default;
};
struct MemberPolicy {
    auth::DeviceId device{};
    AccessRole role = AccessRole::member;
    std::string group = "members";
    bool operator==(const MemberPolicy&) const = default;
};
struct GatewayNode {
    auth::DeviceId device{};
    bool approved = false;
    bool operator==(const GatewayNode&) const = default;
};
struct RoutedSubnet {
    auth::DeviceId gateway{};
    std::uint32_t address = 0;
    std::uint8_t prefix = 24;
    std::string group; // Empty means all members allowed by the gateway ACL.
    bool operator==(const RoutedSubnet&) const = default;
};
struct NetworkPolicy {
    std::uint64_t revision = 0;
    bool managed = false; // Existing networks preserve their original mesh behavior.
    NetworkTopology topology = NetworkTopology::mesh;
    std::vector<auth::DeviceId> hubs;
    std::vector<AccessRule> rules;
    std::vector<std::string> groups;
    std::vector<MemberPolicy> members;
    std::vector<GatewayNode> gateways;
    std::vector<RoutedSubnet> routes;
    bool operator==(const NetworkPolicy&) const = default;
};
[[nodiscard]] std::vector<std::byte> encode_network_policy(const NetworkPolicy& policy);
[[nodiscard]] NetworkPolicy decode_network_policy(std::span<const std::byte> bytes);
[[nodiscard]] AccessRole policy_role(const NetworkPolicy& policy, const auth::DeviceId& device);
[[nodiscard]] std::string_view policy_group(const NetworkPolicy& policy, const auth::DeviceId& device);
[[nodiscard]] bool policy_allows(const NetworkPolicy& policy, const auth::DeviceId& first, const auth::DeviceId& second);
[[nodiscard]] bool private_routed_subnet(std::uint32_t address, std::uint8_t prefix) noexcept;
[[nodiscard]] bool subnet_contains(std::uint32_t subnet, std::uint8_t prefix, std::uint32_t address) noexcept;
[[nodiscard]] bool subnets_overlap(std::uint32_t first, std::uint8_t first_prefix, std::uint32_t second, std::uint8_t second_prefix) noexcept;
}
