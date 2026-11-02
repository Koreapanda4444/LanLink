#include "lanlink/network/posix_tun.hpp"
#include "lanlink/protocol/ipv4.hpp"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace lanlink;
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
bool has_address(std::uint32_t ip) {
    ifaddrs* list=nullptr;require(getifaddrs(&list)==0,"getifaddrs failed");
    bool found=false;
    for(auto* item=list;item;item=item->ifa_next)if(item->ifa_addr && item->ifa_addr->sa_family==AF_INET)
        found=found||ntohl(reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr)==ip;
    freeifaddrs(list);return found;
}
int main() {
    try {
#ifdef __linux__
        require(unshare(CLONE_NEWNET)==0,"TUN test requires an isolated network namespace");
#endif
        network::PosixTunAdapter adapter(1300);
        std::atomic<unsigned> packets=0;
        adapter.start([&](std::span<const std::byte> bytes){
            if(bytes.size()<28 || bytes[9]!=std::byte{17})return;
            auto packet=std::vector<std::byte>(bytes.begin(),bytes.end());
            if(packet[12]!=std::byte{10} || packet[16]!=std::byte{10})return;
            for(unsigned i=0;i<4;++i)std::swap(packet[12+i],packet[16+i]);
            const auto offset=(std::to_integer<unsigned>(packet[0])&15U)*4;
            if(packet.size()<offset+8)return;
            std::swap(packet[offset],packet[offset+2]);std::swap(packet[offset+1],packet[offset+3]);
            packet[offset+6]=packet[offset+7]=std::byte{0};protocol::update_ipv4_checksum(packet);
            adapter.inject(packet);++packets;
        });
        const auto name=adapter.name();require(if_nametoindex(name.c_str())!=0,"TUN was not created");
        protocol::NetworkId id{};id[0]=std::byte{42};
        network::VirtualNetworkAddress address{id,0x0a7ffd01U,0x0a7ffd00U,24};
        adapter.configure(address);adapter.configure(address);
        require(has_address(address.address),"TUN address missing");
        const auto fd=socket(AF_INET,SOCK_DGRAM,0);require(fd>=0,"cannot open UDP socket");
        ifreq info{};std::memcpy(info.ifr_name,name.c_str(),name.size());
        require(ioctl(fd,SIOCGIFMTU,&info)==0 && info.ifr_mtu==1300,"TUN MTU differs");
        timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        sockaddr_in own{};own.sin_family=AF_INET;own.sin_addr.s_addr=htonl(address.address);
        require(bind(fd,reinterpret_cast<sockaddr*>(&own),sizeof(own))==0,"cannot bind virtual UDP");
        auto peer=own;peer.sin_addr.s_addr=htonl(0x0a7ffd02U);peer.sin_port=htons(18888);
        constexpr char message[]="kernel IPv4 TUN round trip";
        require(sendto(fd,message,sizeof(message),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==sizeof(message),"kernel TUN send failed");
        std::array<char,128> reply{};
        require(recv(fd,reply.data(),reply.size(),0)==sizeof(message) && !std::memcmp(message,reply.data(),sizeof(message)),"kernel TUN receive failed");
        ::close(fd);
        require(packets>0 && !adapter.pump_error(),"TUN pump failed");
        network::RoutingConfiguration routes;routes.routes.push_back({id,0xc0a8fb00U,24});
        adapter.configure_routing(routes);adapter.configure_routing({});
        bool unsafe=false;routes.routes[0].address=0;routes.routes[0].prefix=0;
        try{adapter.configure_routing(routes);}catch(const std::invalid_argument&){unsafe=true;}
        require(unsafe,"Internet default route accepted");
        adapter.remove(id);require(!has_address(address.address),"TUN address survived removal");
        adapter.stop();require(if_nametoindex(name.c_str())==0,"TUN survived shutdown");
        std::cout<<"Native TUN: interface, MTU, IPv4 kernel round trip, routes, default-route rejection and cleanup passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
