#pragma once
#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/user_access.hpp"
namespace lanlink::protocol {
[[nodiscard]] LocalMessage create_network_command(std::string_view name,std::string_view password,bool require_approval);
[[nodiscard]] LocalMessage join_network_command(std::string_view target,std::string_view password);
}
