#include "lanlink/network/routed_network.hpp"
#include "lanlink/protocol/ipv4.hpp"
#include <algorithm>
#include <stdexcept>
namespace lanlink::network {
RoutingConfiguration routing_configuration(const session::NetworkSnapshot& snapshot,bool accept_routes,bool forward) {
    RoutingConfiguration result;
    if(!snapshot.identity)return result;
    const auto own=snapshot.identity->device_id;
    for(const auto& n:snapshot.networks)if(n.peers.policy)for(const auto& route:n.peers.policy->routes) {
        const auto& policy=*n.peers.policy;
        if(route.gateway==own && forward) {
            for(const auto& peer:n.peers.peers)if(peer.signed_key && protocol::route_allows(policy,route,peer.device_id))
                result.forwards.push_back({n.peers.network_id,route.address,route.prefix,peer.ipv4_address});
        } else if(accept_routes && protocol::route_allows(policy,route,own) &&
            std::any_of(n.peers.peers.begin(),n.peers.peers.end(),[&](const auto& peer){return peer.device_id==route.gateway && peer.signed_key;})) {
            result.routes.push_back({n.peers.network_id,route.address,route.prefix});
        }
    }
    if(result.forwards.size()>1024)throw std::invalid_argument("gateway forwarding limit exceeded");
    for(std::size_t i=0;i<result.routes.size();++i) {
        const auto& route=result.routes[i];
        if(!protocol::private_routed_subnet(route.address,route.prefix))throw std::invalid_argument("unsafe routed subnet");
        for(const auto& n:snapshot.networks)if(protocol::subnets_overlap(route.address,route.prefix,n.peers.subnet_address,n.peers.prefix_length))throw std::invalid_argument("routed subnet overlaps overlay");
        for(std::size_t j=0;j<i;++j)if(protocol::subnets_overlap(route.address,route.prefix,result.routes[j].address,result.routes[j].prefix))throw std::invalid_argument("routed subnets overlap");
    }
    return result;
}
std::vector<std::byte> encapsulate_routed_ipv4(std::span<const std::byte> inner,std::uint32_t source,std::uint32_t destination) {
    static_cast<void>(protocol::inspect_ipv4(inner));
    if(inner.size()>65515)throw std::length_error("routed IPv4 packet is too large");
    std::vector<std::byte> outer(20);outer[0]=std::byte{0x45};const auto size=inner.size()+20;
    outer[2]=static_cast<std::byte>(size>>8);outer[3]=static_cast<std::byte>(size);outer[8]=std::byte{64};outer[9]=std::byte{4};
    for(unsigned i=0;i<4;++i){outer[12+i]=static_cast<std::byte>(source>>(24-i*8));outer[16+i]=static_cast<std::byte>(destination>>(24-i*8));}
    protocol::update_ipv4_checksum(outer);outer.insert(outer.end(),inner.begin(),inner.end());return outer;
}
std::span<const std::byte> decapsulate_routed_ipv4(std::span<const std::byte> outer) {
    const auto h=protocol::inspect_ipv4(outer);
    if(outer[9]!=std::byte{4} || h.header_size!=20 || h.fragment_offset || h.more_fragments)throw std::invalid_argument("invalid gateway tunnel");
    const auto inner=outer.subspan(20);static_cast<void>(protocol::inspect_ipv4(inner));return inner;
}
}
