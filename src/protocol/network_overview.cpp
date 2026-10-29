#include "lanlink/protocol/network_overview.hpp"
#include <algorithm>
namespace lanlink::protocol {
NetworkOverview network_overview(const NetworkPeerState& state,const PathDiagnostics* paths,bool authenticated) {
    NetworkOverview out;out.total=static_cast<unsigned>(state.peers.size()+1);
    if(!authenticated)return out;
    out.online=1;
    for(const auto& peer:state.peers) {
        if(!peer.signed_key)continue;
        ++out.online;
        if(!paths){++out.connecting;continue;}
        const auto found=std::find_if(paths->peers.begin(),paths->peers.end(),[&](const auto& p){return p.network_id==state.network_id && p.device_id==peer.device_id;});
        if(found==paths->peers.end() || found->path.lifecycle!=PathLifecycle::authenticated){++out.connecting;continue;}
        if(found->path.kind==PathKind::direct_quic)++out.direct;
        else if(found->path.kind==PathKind::relay_quic || found->path.kind==PathKind::relay_tls)++out.relayed;
        else ++out.connecting;
    }
    return out;
}
}
