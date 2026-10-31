#include "lanlink/protocol/policy_preview.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>
namespace lanlink::protocol {
std::pair<std::uint32_t,std::uint8_t> parse_private_cidr(std::string_view text) {
    const auto fail=[](){throw std::invalid_argument("Enter a canonical private IPv4 subnet.");};
    std::uint32_t address=0;std::size_t offset=0;
    for(unsigned i=0;i<4;++i){const auto end=text.find(i==3?'/':'.',offset);if(end==std::string_view::npos || end==offset)fail();unsigned octet=0;
        const auto result=std::from_chars(text.data()+offset,text.data()+end,octet);if(result.ec!=std::errc{} || result.ptr!=text.data()+end || octet>255)fail();address=(address<<8)|octet;offset=end+1;}
    unsigned prefix=0;const auto result=std::from_chars(text.data()+offset,text.data()+text.size(),prefix);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || prefix>32 || !private_routed_subnet(address,static_cast<std::uint8_t>(prefix)) || subnets_overlap(address,static_cast<std::uint8_t>(prefix),0x0a400000U,10))fail();
    return {address,static_cast<std::uint8_t>(prefix)};
}
PolicyPreview preview_network_policy(const NetworkPeerState& peers,const auth::DeviceId& own,const NetworkPolicy& p) {
    static_cast<void>(encode_network_policy(p));
    if(p.managed && p.topology==NetworkTopology::hub_spoke && p.hubs.empty())throw std::invalid_argument("Select at least one hub.");
    std::vector<auth::DeviceId> members{own};for(const auto& peer:peers.peers)members.push_back(peer.device_id);
    const auto member=[&](const auth::DeviceId& id){return std::find(members.begin(),members.end(),id)!=members.end();};
    for(const auto& hub:p.hubs)if(!member(hub))throw std::invalid_argument("A selected device is no longer a member.");
    for(const auto& gateway:p.gateways)if(!member(gateway.device))throw std::invalid_argument("A selected device is no longer a member.");
    for(const auto& rule:p.rules)for(const auto* subject:{&rule.first,&rule.second})if(subject->device!=auth::DeviceId{} && !member(subject->device))throw std::invalid_argument("A selected device is no longer a member.");
    PolicyPreview out;
    for(std::size_t i=0;i<members.size();++i){if(policy_role(p,members[i])!=(peers.policy?policy_role(*peers.policy,members[i]):AccessRole::member))++out.changed_roles;
        for(std::size_t j=i+1;j<members.size();++j){if(policy_allows(p,members[i],members[j]))++out.allowed_pairs;else ++out.blocked_pairs;}}
    out.gateways=static_cast<unsigned>(std::count_if(p.gateways.begin(),p.gateways.end(),[](const auto& g){return g.approved;}));
    out.routes=static_cast<unsigned>(p.routes.size());
    for(const auto& route:p.routes)if(subnets_overlap(route.address,route.prefix,0x0a400000U,10) || subnets_overlap(route.address,route.prefix,peers.subnet_address,peers.prefix_length))throw std::invalid_argument("The routed subnet overlaps a virtual network.");
    return out;
}
}
