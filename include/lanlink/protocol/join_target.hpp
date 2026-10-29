#pragma once
#include "lanlink/protocol/network_messages.hpp"
namespace lanlink::protocol {
struct JoinTarget { NetworkId network_id{}; std::string invitation; };
[[nodiscard]] JoinTarget parse_join_target(std::string_view text);
[[nodiscard]] std::string make_invite_link(const NetworkId& network,std::string_view secret);
[[nodiscard]] std::string make_invite_code(const NetworkId& network,std::string_view secret);
}
