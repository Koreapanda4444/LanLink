#include "lanlink/protocol/policy_preview.hpp"
#include <iostream>
#include <stdexcept>
using namespace lanlink::protocol;
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(){try{
    NetworkPeerState peers;peers.network_id[0]=std::byte{1};peers.subnet_address=0x0a400000;peers.own_address=0x0a400001;
    lanlink::auth::DeviceId own{std::byte{1}},a{std::byte{2}},b{std::byte{3}};peers.peers={{a,0x0a400002},{b,0x0a400003}};
    NetworkPolicy p;p.managed=true;p.rules.push_back({{}, {}, true});
    auto preview=preview_network_policy(peers,own,p);check(preview.allowed_pairs==3 && preview.blocked_pairs==0,"mesh preview");
    p.topology=NetworkTopology::hub_spoke;p.hubs={own};preview=preview_network_policy(peers,own,p);check(preview.allowed_pairs==2 && preview.blocked_pairs==1,"hub preview");
    p.rules.clear();preview=preview_network_policy(peers,own,p);check(preview.blocked_pairs==3,"default deny preview");
    check(parse_private_cidr("192.168.50.0/24")==std::pair<std::uint32_t,std::uint8_t>{0xc0a83200U,24},"CIDR parser");
    for(const auto* invalid:{"0.0.0.0/0","8.8.8.0/24","10.64.0.0/24","192.168.50.1/24","192.168.1.0/99","192.168.1.0/24junk"}){bool rejected=false;try{static_cast<void>(parse_private_cidr(invalid));}catch(const std::exception&){rejected=true;}check(rejected,"unsafe route preview accepted");}
    std::cout<<"policy connectivity preview and private CIDR validation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
