#pragma once
#include "lanlink/protocol/path_diagnostics.hpp"
namespace lanlink::protocol {
struct NetworkOverview {
    unsigned total = 0, online = 0, direct = 0, relayed = 0, connecting = 0;
};
[[nodiscard]] NetworkOverview network_overview(const NetworkPeerState& state,const PathDiagnostics* paths,bool authenticated);
}
