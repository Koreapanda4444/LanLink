#include "lanlink/p2p/local_candidates.hpp"
#include <iostream>
#include <stdexcept>
using namespace lanlink::p2p;
int main() { try {
    std::vector<InterfaceAddress> interfaces{
        {"ethernet",0xc0a80102,2,true,false,false},{"wifi",0x0a010203,5,true,false,false},
        {"duplicate",0xc0a80102,9,true,false,false},{"loopback",0x7f000001,1,true,true,false},
        {"LanLink",0x0a400001,6,true,false,false},{"tunnel",0xc0a80201,8,true,false,true},
        {"down",0xc0a80301,3,false,false,false},{"linklocal",0xa9fe0102,4,true,false,false},
        {"multicast",0xe0010203,4,true,false,false},{"any",0,4,true,false,false}};
    const std::array excluded{ExcludedSubnet{0x0a400000,24}};
    const auto candidates=gather_local_candidates(interfaces,4434,excluded);
    if (candidates.size()!=2 || candidates[0].endpoint.ipv4!=0xc0a80102 || candidates[1].endpoint.ipv4!=0x0a010203)
        throw std::runtime_error("NIC filtering, deduplication or priority failed");
    if (gather_local_candidates(interfaces,4434,excluded,true).size()!=3 || !gather_local_candidates(interfaces,0,excluded).empty())
        throw std::runtime_error("loopback override or invalid port failed");
    const auto native=interface_addresses();
    const auto gathered=gather_local_candidates(native,4434);
    if (gathered.size()>max_candidates) throw std::runtime_error("unbounded native candidates");
    if (native.empty()) std::cout<<"no native IPv4 interfaces in this environment; synthetic NIC matrix checked\n";
    std::cout<<"candidate filtering and native enumeration passed\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
