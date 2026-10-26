#include "lanlink/p2p/local_candidates.hpp"
#include <algorithm>
#include <memory>
#include <set>
#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace lanlink::p2p {
std::vector<InterfaceAddress> interface_addresses() {
    std::vector<InterfaceAddress> result;
#ifdef _WIN32
    ULONG size=16*1024;
    std::vector<unsigned char> buffer(size);
    auto* adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG error=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,adapters,&size);
    if (error==ERROR_BUFFER_OVERFLOW && size<=1024*1024) {
        buffer.resize(size); adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        error=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,adapters,&size);
    }
    if (error!=NO_ERROR) return result;
    for (auto* a=adapters;a;a=a->Next) for (auto* u=a->FirstUnicastAddress;u;u=u->Next) {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family!=AF_INET) continue;
        const auto* addr=reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
        result.push_back({a->AdapterName? a->AdapterName:"",ntohl(addr->sin_addr.s_addr),a->IfIndex,
            a->OperStatus==IfOperStatusUp,a->IfType==IF_TYPE_SOFTWARE_LOOPBACK,a->IfType==IF_TYPE_TUNNEL});
    }
#else
    ifaddrs* raw=nullptr; if (getifaddrs(&raw)!=0) return result;
    std::unique_ptr<ifaddrs,decltype(&freeifaddrs)> guard(raw,freeifaddrs);
    for (auto* a=raw;a;a=a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family!=AF_INET) continue;
        const auto* addr=reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        std::string name=a->ifa_name;
        const bool tunnel=name.starts_with("tun") || name.starts_with("utun") || name.starts_with("lanlink");
        result.push_back({name,ntohl(addr->sin_addr.s_addr),if_nametoindex(a->ifa_name),
            (a->ifa_flags & IFF_UP)!=0,(a->ifa_flags & IFF_LOOPBACK)!=0,tunnel});
    }
#endif
    return result;
}
std::vector<Candidate> gather_local_candidates(std::span<const InterfaceAddress> interfaces,std::uint16_t port,
    std::span<const ExcludedSubnet> excluded,bool loopback) {
    std::vector<Candidate> result; std::set<Endpoint> seen;
    for (const auto& i : interfaces) {
        if (!i.up || i.tunnel || (!loopback && i.loopback) || !usable_endpoint({i.ipv4,port},loopback)) continue;
        bool skip=false;
        for (auto subnet:excluded) {
            if (subnet.prefix>32) throw std::invalid_argument("invalid excluded subnet");
            const auto mask=subnet.prefix ? 0xffffffffU << (32-subnet.prefix) : 0U;
            if ((i.ipv4&mask)==(subnet.address&mask)) skip=true;
        }
        if (skip || !seen.insert({i.ipv4,port}).second) continue;
        const bool private_address=(i.ipv4>>24)==10 || (i.ipv4&0xfff00000U)==0xac100000U || (i.ipv4&0xffff0000U)==0xc0a80000U;
        // ICE type preference, stable interface preference, component 1.
        const auto local=65535U-std::min(i.index,65535U);
        const auto priority=((i.loopback?100U:private_address?126U:120U)<<24) | (local<<8) | 255U;
        result.push_back({{i.ipv4,port},CandidateKind::host,priority});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.priority!=b.priority?a.priority>b.priority:a.endpoint<b.endpoint;});
    if (result.size()>max_candidates) result.resize(max_candidates);
    return result;
}
}
