#include "lanlink/p2p/stun.hpp"
#include "lanlink/protocol/binary.hpp"
#include <openssl/rand.h>
namespace lanlink::p2p {
namespace {constexpr std::uint32_t cookie=0x2112a442U;}
StunTransaction new_stun_transaction() {
    StunTransaction t{};if(RAND_bytes(reinterpret_cast<unsigned char*>(t.data()),static_cast<int>(t.size()))!=1)throw std::runtime_error("STUN random generation failed");return t;
}
std::vector<std::byte> stun_binding_request(const StunTransaction& t) {
    protocol::BinaryWriter w;w.put<std::uint16_t>(0x0001);w.put<std::uint16_t>(0);w.put(cookie);w.append(t);return std::move(w).finish();
}
Endpoint parse_stun_binding_response(std::span<const std::byte> b,const StunTransaction& t) {
    if(b.size()<20 || b.size()>1200)throw std::invalid_argument("invalid STUN size");
    protocol::BinaryReader r(b);
    if(r.get<std::uint16_t>()!=0x0101)throw std::invalid_argument("not a STUN binding success");
    const auto size=r.get<std::uint16_t>();
    if(size%4 || size!=b.size()-20 || r.get<std::uint32_t>()!=cookie || r.array<12>()!=t)throw std::invalid_argument("invalid STUN transaction");
    std::optional<Endpoint> mapped;
    for(std::size_t offset=20;offset<b.size();) {
        const auto type=r.get<std::uint16_t>(),length=r.get<std::uint16_t>();
        const auto value=r.bytes(length); static_cast<void>(r.bytes((4-length%4)%4)); offset+=4+((length+3U)&~3U);
        if(type==0x0020) {
            if(mapped || length!=8)throw std::invalid_argument("invalid XOR-MAPPED-ADDRESS");
            protocol::BinaryReader a(value);
            if(a.get<std::uint8_t>()!=0 || a.get<std::uint8_t>()!=1)throw std::invalid_argument("unsupported STUN address family");
            Endpoint e{0,static_cast<std::uint16_t>(a.get<std::uint16_t>()^(cookie>>16))};
            e.ipv4=a.get<std::uint32_t>()^cookie;
            if(!usable_endpoint(e))throw std::invalid_argument("invalid STUN mapped endpoint");
            mapped=e;
        }
    }
    r.finish();if(!mapped)throw std::invalid_argument("missing XOR-MAPPED-ADDRESS");return *mapped;
}
std::optional<Candidate> discover_stun_endpoint(UdpSocket& socket,Endpoint server,std::chrono::milliseconds timeout) {
    if(!usable_endpoint(server) || timeout.count()<=0)return std::nullopt;
    timeout=std::min(timeout,std::chrono::milliseconds{2000});
    const auto transaction=new_stun_transaction(); const auto request=stun_binding_request(transaction);
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    for(unsigned attempt=0;attempt<3 && std::chrono::steady_clock::now()<deadline;++attempt) {
        if(!socket.send(server,request))return std::nullopt;
        const auto end=std::min(deadline,std::chrono::steady_clock::now()+timeout/3);
        while(std::chrono::steady_clock::now()<end) {
            auto response=socket.receive(std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()));
            if(!response || response->source!=server)continue;
            try {return Candidate{parse_stun_binding_response(response->bytes,transaction),CandidateKind::server_reflexive,(100U<<24)|255U};}
            catch(const std::invalid_argument&){}
        }
    }
    return std::nullopt;
}
}
