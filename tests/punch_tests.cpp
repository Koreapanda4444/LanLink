#include "lanlink/p2p/punch.hpp"
#include <iostream>
#include <future>
using namespace lanlink;
namespace {
auth::SignedDeviceKey key(const auth::DeviceIdentity& i) {
    auth::SignedDeviceKey k;k.identity_public_key=i.public_key();k.encryption_public_key=auth::DeviceEncryptionKey::generate().public_key();
    k.client_nonce=auth::random_nonce();k.server_nonce=auth::random_nonce();k.signature=i.sign(auth::make_auth_transcript(k.identity_public_key,k.encryption_public_key,k.client_nonce,k.server_nonce));return k;
}
void require(bool b,const char* reason){if(!b)throw std::runtime_error(reason);}
}
int main(){const auto root=std::filesystem::temp_directory_path()/("lanlink-punch-"+std::to_string(p2p::PunchSession::Clock::now().time_since_epoch().count()));try {
    const auto a=auth::DeviceIdentity::load_or_create(root/"a"),b=auth::DeviceIdentity::load_or_create(root/"b");const auto ak=key(a),bk=key(b);
    p2p::PeerBinding binding;binding.network_id[0]=std::byte{1};binding.key_epoch=2;binding.local_generation=binding.peer_generation=1;
    binding.local_device=a.device_id();binding.peer_device=b.device_id();binding.local_session=ak.encryption_public_key;binding.peer_session=bk.encryption_public_key;
    p2p::UdpSocket as({0x7f000001,0}),bs({0x7f000001,0});
    const std::vector<p2p::Candidate> ac{{as.local_endpoint(),p2p::CandidateKind::host,100}},bc{{bs.local_endpoint(),p2p::CandidateKind::host,100}};
    p2p::PunchSession ap(a,bk,binding,bc),bp(b,ak,binding.reversed(),ac);
    auto af=std::async(std::launch::async,[&]{return p2p::run_punch(as,ap);});
    const auto br=p2p::run_punch(bs,bp),ar=af.get();require(ar==bs.local_endpoint() && br==as.local_endpoint(),"real UDP mutual punching failed");
    const auto start=p2p::PunchSession::Clock::now();
    // Deterministic NAT simulation: full cone, restricted cone, symmetric mappings, blocked UDP.
    for(unsigned mode=0;mode<4;++mode) {
        p2p::PunchSession left(a,bk,binding,bc,start),right(b,ak,binding.reversed(),ac,start);
        bool opened_left=false,opened_right=false;
        for(unsigned ms=0;ms<=5100;ms+=25) {
            const auto now=start+std::chrono::milliseconds{ms};auto l=left.tick(now),r=right.tick(now);
            opened_left|=!l.empty();opened_right|=!r.empty();
            auto deliver=[&](const auto& outgoing,auto& target,p2p::Endpoint source,auto& sender,p2p::Endpoint reply_source,bool open){
                for(const auto& d:outgoing){if(mode==3 || (mode==1 && !open))continue;
                    auto mapped=source; if(mode==2)mapped.port=static_cast<std::uint16_t>(source.port==65535?60000:source.port+1);
                    const auto response=target.receive({mapped,d.bytes},now);
                    if(response)static_cast<void>(sender.receive({reply_source,response->bytes},now));}
            };
            deliver(l,right,as.local_endpoint(),left,bs.local_endpoint(),opened_right);
            deliver(r,left,bs.local_endpoint(),right,as.local_endpoint(),opened_left);
        }
        require(left.selected().has_value()==(mode<2) && right.selected().has_value()==(mode<2),"NAT simulation selected incorrect path");
        require(left.sent_probes()<=32 && right.sent_probes()<=32,"unbounded punch retries");
    }
    p2p::PunchSession replay(a,bk,binding,bc,start),peer(b,ak,binding.reversed(),ac,start);
    auto probe=peer.tick(start).front().bytes;probe.back()^=std::byte{1};
    require(!replay.receive({bs.local_endpoint(),probe},start),"forged probe produced a response");
    std::filesystem::remove_all(root);std::cout<<"real authenticated UDP and simulated NAT matrix passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::error_code ignored;std::filesystem::remove_all(root,ignored);return 1;}}
