#pragma once
#include "lanlink/protocol/network_messages.hpp"
namespace lanlink::protocol {
struct PolicyPreview {
    unsigned allowed_pairs=0, blocked_pairs=0, gateways=0, routes=0, changed_roles=0;
};
[[nodiscard]] PolicyPreview preview_network_policy(const NetworkPeerState& peers,const auth::DeviceId& own,const NetworkPolicy& policy);
[[nodiscard]] std::pair<std::uint32_t,std::uint8_t> parse_private_cidr(std::string_view text);
}
