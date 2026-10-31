#include "lanlink/protocol/network_policy.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace lanlink::protocol {
namespace {
void require(bool ok) { if (!ok) throw std::invalid_argument("invalid network policy"); }
void text(BinaryWriter& w, const std::string& value) {
    require(value.size() <= 32 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }));
    w.put<std::uint8_t>(static_cast<std::uint8_t>(value.size()));
    w.append(std::as_bytes(std::span(value.data(),value.size())));
}
std::string text(BinaryReader& r) {
    const auto bytes=r.bytes(r.get<std::uint8_t>());
    return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
void subject(BinaryWriter& w,const PolicySubject& s) {
    require(s.device==auth::DeviceId{} || s.group.empty());w.append(s.device);text(w,s.group);
}
PolicySubject subject(BinaryReader& r) { return {r.array<32>(),text(r)}; }
bool matches(const NetworkPolicy& p,const PolicySubject& s,const auth::DeviceId& d) {
    return s.device!=auth::DeviceId{} ? s.device==d : s.group.empty() || s.group==policy_group(p,d);
}
}
bool subnet_contains(std::uint32_t subnet,std::uint8_t prefix,std::uint32_t address) noexcept {
    return prefix>0 && prefix<=32 && (address & (0xffffffffU << (32U-prefix)))==subnet;
}
bool subnets_overlap(std::uint32_t a,std::uint8_t ap,std::uint32_t b,std::uint8_t bp) noexcept {
    if (!ap || ap>32 || !bp || bp>32) return true;
    const auto mask=0xffffffffU << (32U-std::min(ap,bp));return (a&mask)==(b&mask);
}
bool private_routed_subnet(std::uint32_t a,std::uint8_t p) noexcept {
    if(p<8 || p>30 || !subnet_contains(a,p,a))return false;
    return (p>=8 && subnet_contains(0x0a000000U,8,a)) ||
        (p>=12 && subnet_contains(0xac100000U,12,a)) || (p>=16 && subnet_contains(0xc0a80000U,16,a));
}
AccessRole policy_role(const NetworkPolicy& p,const auth::DeviceId& d) {
    for(const auto& m:p.members)if(m.device==d)return m.role;
    return AccessRole::member;
}
std::string_view policy_group(const NetworkPolicy& p,const auth::DeviceId& d) {
    for(const auto& m:p.members)if(m.device==d)return m.group;
    return "members";
}
bool policy_fanout_allows(const NetworkPolicy& p,const auth::DeviceId& a,const auth::DeviceId& b) {
    return policy_allows(p,a,b) && (!p.managed || (policy_role(p,a)!=AccessRole::restricted && policy_role(p,b)!=AccessRole::restricted));
}
bool route_allows(const NetworkPolicy& p,const RoutedSubnet& r,const auth::DeviceId& member) {
    return p.managed && policy_role(p,member)!=AccessRole::restricted &&
        (r.group.empty() || policy_group(p,member)==r.group) && policy_allows(p,member,r.gateway) &&
        std::any_of(p.gateways.begin(),p.gateways.end(),[&](const auto& g){return g.device==r.gateway && g.approved;});
}
bool policy_allows(const NetworkPolicy& p,const auth::DeviceId& a,const auth::DeviceId& b) {
    if(a==b || a==auth::DeviceId{} || b==auth::DeviceId{})return false;
    if(!p.managed)return true;
    if(p.topology==NetworkTopology::hub_spoke &&
        std::find(p.hubs.begin(),p.hubs.end(),a)==p.hubs.end() &&
        std::find(p.hubs.begin(),p.hubs.end(),b)==p.hubs.end())return false;
    bool allowed=false;
    for(const auto& rule:p.rules) {
        if((matches(p,rule.first,a) && matches(p,rule.second,b)) ||
           (matches(p,rule.first,b) && matches(p,rule.second,a))) {
            if(!rule.allow)return false;
            allowed=true;
        }
    }
    return allowed;
}
std::vector<std::byte> encode_network_policy(const NetworkPolicy& p) {
    require(p.topology==NetworkTopology::mesh || p.topology==NetworkTopology::hub_spoke);
    require(p.hubs.size()<=32 && p.rules.size()<=64 && p.groups.size()<=16 && p.members.size()<=254 && p.gateways.size()<=16 && p.routes.size()<=32);
    require(p.managed || (p.topology==NetworkTopology::mesh && p.hubs.empty() && p.rules.empty() && p.groups.empty() && p.members.empty() && p.gateways.empty() && p.routes.empty()));
    std::set<auth::DeviceId> ids;
    BinaryWriter w;w.put<std::uint8_t>(1);w.put<std::uint64_t>(p.revision);w.put<std::uint8_t>(p.managed?1:0);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(p.topology));w.put<std::uint8_t>(static_cast<std::uint8_t>(p.hubs.size()));
    for(const auto& d:p.hubs){require(d!=auth::DeviceId{} && ids.insert(d).second);w.append(d);}
    w.put<std::uint8_t>(static_cast<std::uint8_t>(p.rules.size()));
    for(const auto& r:p.rules){subject(w,r.first);subject(w,r.second);w.put<std::uint8_t>(r.allow?1:0);}
    std::set<std::string> groups{"members"};
    w.put<std::uint8_t>(static_cast<std::uint8_t>(p.groups.size()));
    for(const auto& g:p.groups){require(!g.empty() && g!="members" && groups.insert(g).second);text(w,g);}
    ids.clear();w.put<std::uint16_t>(static_cast<std::uint16_t>(p.members.size()));
    for(const auto& m:p.members){require(m.device!=auth::DeviceId{} && ids.insert(m.device).second && m.role<=AccessRole::restricted && groups.contains(m.group));w.append(m.device);w.put<std::uint8_t>(static_cast<std::uint8_t>(m.role));text(w,m.group);}
    for(const auto& r:p.rules)for(const auto* s:{&r.first,&r.second})require(s->group.empty() || groups.contains(s->group));
    ids.clear();w.put<std::uint8_t>(static_cast<std::uint8_t>(p.gateways.size()));
    for(const auto& g:p.gateways){require(g.device!=auth::DeviceId{} && ids.insert(g.device).second);w.append(g.device);w.put<std::uint8_t>(g.approved?1:0);}
    w.put<std::uint8_t>(static_cast<std::uint8_t>(p.routes.size()));
    for(std::size_t i=0;i<p.routes.size();++i){const auto& r=p.routes[i];require(private_routed_subnet(r.address,r.prefix) && (r.group.empty() || groups.contains(r.group)));
        require(std::any_of(p.gateways.begin(),p.gateways.end(),[&](const auto& g){return g.device==r.gateway && g.approved;}));
        for(std::size_t j=0;j<i;++j)require(!subnets_overlap(r.address,r.prefix,p.routes[j].address,p.routes[j].prefix));
        w.append(r.gateway);w.put<std::uint32_t>(r.address);w.put<std::uint8_t>(r.prefix);text(w,r.group);
    }
    return std::move(w).finish();
}
NetworkPolicy decode_network_policy(std::span<const std::byte> bytes) {
    require(bytes.size()<=32768);BinaryReader r(bytes);require(r.get<std::uint8_t>()==1);
    NetworkPolicy p;p.revision=r.get<std::uint64_t>();const auto managed=r.get<std::uint8_t>();require(managed<=1);p.managed=managed!=0;
    p.topology=static_cast<NetworkTopology>(r.get<std::uint8_t>());
    auto count=r.get<std::uint8_t>();require(count<=32);for(unsigned i=0;i<count;++i)p.hubs.push_back(r.array<32>());
    count=r.get<std::uint8_t>();require(count<=64);for(unsigned i=0;i<count;++i){AccessRule rule;rule.first=subject(r);rule.second=subject(r);const auto f=r.get<std::uint8_t>();require(f<=1);rule.allow=f!=0;p.rules.push_back(rule);}
    count=r.get<std::uint8_t>();require(count<=16);for(unsigned i=0;i<count;++i)p.groups.push_back(text(r));
    const auto members=r.get<std::uint16_t>();require(members<=254);for(unsigned i=0;i<members;++i){MemberPolicy m;m.device=r.array<32>();m.role=static_cast<AccessRole>(r.get<std::uint8_t>());m.group=text(r);p.members.push_back(m);}
    count=r.get<std::uint8_t>();require(count<=16);for(unsigned i=0;i<count;++i){GatewayNode g;g.device=r.array<32>();const auto f=r.get<std::uint8_t>();require(f<=1);g.approved=f!=0;p.gateways.push_back(g);}
    count=r.get<std::uint8_t>();require(count<=32);for(unsigned i=0;i<count;++i){RoutedSubnet s;s.gateway=r.array<32>();s.address=r.get<std::uint32_t>();s.prefix=r.get<std::uint8_t>();s.group=text(r);p.routes.push_back(s);}
    r.finish();static_cast<void>(encode_network_policy(p));return p;
}
}
