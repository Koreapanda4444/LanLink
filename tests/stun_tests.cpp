#include "lanlink/p2p/stun.hpp"
#include "lanlink/protocol/binary.hpp"
#include <thread>
#include <iostream>
using namespace lanlink;
int main(){try {
    p2p::UdpSocket server({0x7f000001,0}),client({0x7f000001,0});
    std::jthread worker([&](std::stop_token stop){while(!stop.stop_requested()) {
        const auto request=server.receive(std::chrono::milliseconds{20});if(!request)continue;
        protocol::BinaryReader r(request->bytes);if(r.get<std::uint16_t>()!=1)continue;
        static_cast<void>(r.get<std::uint16_t>());static_cast<void>(r.get<std::uint32_t>());const auto tx=r.array<12>();
        protocol::BinaryWriter w;w.put<std::uint16_t>(0x0101);w.put<std::uint16_t>(12);w.put<std::uint32_t>(0x2112a442);w.append(tx);
        w.put<std::uint16_t>(0x20);w.put<std::uint16_t>(8);w.put<std::uint8_t>(0);w.put<std::uint8_t>(1);
        w.put<std::uint16_t>(request->source.port^0x2112);w.put<std::uint32_t>(request->source.ipv4^0x2112a442);
        static_cast<void>(server.send(request->source,std::move(w).finish()));
    }});
    const auto result=p2p::discover_stun_endpoint(client,server.local_endpoint(),std::chrono::milliseconds{500});
    if(!result || result->endpoint!=client.local_endpoint() || result->kind!=p2p::CandidateKind::server_reflexive)throw std::runtime_error("live STUN discovery failed");
    worker.request_stop();worker.join();
    if(p2p::discover_stun_endpoint(client,server.local_endpoint(),std::chrono::milliseconds{30}))throw std::runtime_error("STUN timeout returned an endpoint");
    auto t=p2p::new_stun_transaction();auto request=p2p::stun_binding_request(t);
    for(std::size_t n=0;n<=request.size();++n){bool rejected=false;try{static_cast<void>(p2p::parse_stun_binding_response(std::span(request).first(n),t));}catch(const std::exception&){rejected=true;}if(!rejected)throw std::runtime_error("malformed STUN response accepted");}
    protocol::BinaryWriter w;w.put<std::uint16_t>(0x0101);w.put<std::uint16_t>(12);w.put<std::uint32_t>(0x2112a442);w.append(t);
    w.put<std::uint16_t>(0x20);w.put<std::uint16_t>(8);w.put<std::uint8_t>(0);w.put<std::uint8_t>(1);w.put<std::uint16_t>(45000^0x2112);w.put<std::uint32_t>(0xcb007105^0x2112a442);
    const auto response=std::move(w).finish();
    if(p2p::parse_stun_binding_response(response,t)!=p2p::Endpoint{0xcb007105,45000})throw std::runtime_error("XOR mapped address decoded incorrectly");
    for(unsigned field: {0U,2U,4U,8U,22U,25U}) {auto invalid=response;invalid[field]^=std::byte{1};bool rejected=false;
        try{static_cast<void>(p2p::parse_stun_binding_response(invalid,t));}catch(const std::exception&){rejected=true;}
        if(!rejected)throw std::runtime_error("corrupt STUN success accepted");}
    std::cout<<"live STUN, source/transaction validation and timeout passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
