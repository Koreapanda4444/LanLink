#include "lanlink/p2p/direct_quic.hpp"
#include "lanlink/auth/packet_crypto.hpp"
#include <iostream>
#include <thread>
#include <mutex>
#include <future>
using namespace lanlink;
namespace {
void require(bool b,const char* reason){if(!b)throw std::runtime_error(reason);}
auth::SignedDeviceKey key(const auth::DeviceIdentity& i){auth::SignedDeviceKey k;k.identity_public_key=i.public_key();k.encryption_public_key=auth::DeviceEncryptionKey::generate().public_key();k.client_nonce=auth::random_nonce();k.server_nonce=auth::random_nonce();k.signature=i.sign(auth::make_auth_transcript(k.identity_public_key,k.encryption_public_key,k.client_nonce,k.server_nonce));return k;}
template<class F>void wait(F ready,const char* reason){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds{8};while(!ready()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error(reason);std::this_thread::sleep_for(std::chrono::milliseconds{5});}}
std::vector<std::byte> ipv4(std::size_t size,bool reverse=false){std::vector<std::byte> p(size);p[0]=std::byte{0x45};p[2]=static_cast<std::byte>(size>>8);p[3]=static_cast<std::byte>(size&255);p[8]=std::byte{64};p[9]=std::byte{17};p[12]=p[16]=std::byte{10};p[13]=p[17]=std::byte{64};p[15]=std::byte{static_cast<unsigned char>(reverse?2:1)};p[19]=std::byte{static_cast<unsigned char>(reverse?1:2)};return p;}
}
int main(){const auto root=std::filesystem::temp_directory_path()/("lanlink-direct-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{
    const auto a=auth::DeviceIdentity::load_or_create(root/"a"),b=auth::DeviceIdentity::load_or_create(root/"b");const auto ak=key(a),bk=key(b);
    p2p::PeerBinding binding;binding.network_id[0]=std::byte{1};binding.key_epoch=1;binding.local_generation=binding.peer_generation=1;
    binding.local_device=a.device_id();binding.peer_device=b.device_id();binding.local_session=ak.encryption_public_key;binding.peer_session=bk.encryption_public_key;
    const auto network_key=auth::NetworkKey::random();
    auth::NetworkPacketCipher ca(binding.network_id,1,network_key,a.device_id(),ak,0x0a400001),cb(binding.network_id,1,network_key,b.device_id(),bk,0x0a400002);
    ca.update_peers({binding.network_id,1,0x0a400000,24,0x0a400001,{{b.device_id(),0x0a400002,bk}},a.device_id(),1});
    cb.update_peers({binding.network_id,1,0x0a400000,24,0x0a400002,{{a.device_id(),0x0a400001,ak}},a.device_id(),1});
    auto udp_a=std::make_unique<p2p::UdpSocket>(p2p::Endpoint{0x7f000001,0}),udp_b=std::make_unique<p2p::UdpSocket>(p2p::Endpoint{0x7f000001,0});
    const auto port_a=udp_a->local_endpoint().port,port_b=udp_b->local_endpoint().port;
    p2p::PunchSession punch_a(a,bk,binding,{{{0x7f000001,port_b},p2p::CandidateKind::host,100}}),
        punch_b(b,ak,binding.reversed(),{{{0x7f000001,port_a},p2p::CandidateKind::host,100}});
    auto probing=std::async(std::launch::async,[&]{return p2p::run_punch(*udp_a,punch_a);});
    const auto mapped_b=p2p::run_punch(*udp_b,punch_b),mapped_a=probing.get();
    require(mapped_a && mapped_b,"authenticated UDP checks before QUIC failed");udp_a.reset();udp_b.reset();
    std::mutex mutex;std::vector<std::vector<std::byte>> left,right;unsigned replay_drops=0;
    p2p::DirectQuic first(a,{binding,bk,0x0a400001,0x0a400002,port_a},[&](const auto& d){std::lock_guard lock(mutex);left.push_back(ca.decrypt(d.packet,d.sender_device_id));});
    p2p::DirectQuic second(b,{binding.reversed(),ak,0x0a400002,0x0a400001,port_b},[&](const auto& d){std::lock_guard lock(mutex);try{right.push_back(cb.decrypt(d.packet,d.sender_device_id));}catch(const std::exception&){++replay_drops;}});
    first.start();second.start();first.connect({0x7f000001,second.port()});second.connect({0x7f000001,first.port()});
    wait([&]{return first.authenticated() && second.authenticated();},"direct mutual device proof failed");
    const auto small=ipv4(40),large=ipv4(4000),reverse=ipv4(40,true);const auto packet=ca.encrypt(small);
    require(packet.ciphertext!=small,"direct packet was not end-to-end encrypted");
    require(first.send(packet)==transport::PacketSendResult::accepted && first.send(ca.encrypt(large))==transport::PacketSendResult::accepted,"direct data send failed");
    require(second.send(cb.encrypt(reverse))==transport::PacketSendResult::accepted,"reverse direct data failed");
    wait([&]{std::lock_guard lock(mutex);return right.size()==2 && left.size()==1;},"direct datagram/stream encrypted round trip failed");
    {std::lock_guard lock(mutex);require(right[0]==small && right[1]==large && left[0]==reverse,"direct plaintext changed");}
    require(first.send(packet)==transport::PacketSendResult::accepted,"duplicate fixture send failed");
    wait([&]{std::lock_guard lock(mutex);return replay_drops==1;},"direct packet replay was not rejected");
    const auto stats=first.status();require(stats.kind==transport::PathKind::direct_quic && stats.rtt_us && stats.sent_packets==3 && stats.received_packets==1,"direct telemetry is not measured");
    second.stop();wait([&]{return !first.authenticated();},"closed direct path retained authorization");first.stop();
    // An untrusted identity cannot use a valid network binding to impersonate an approved peer.
    auto wrong=bk;wrong.signature[0]^=std::byte{1};bool rejected=false;
    try{p2p::DirectQuic invalid(a,{binding,wrong,0x0a400001,0x0a400002,0},{});}catch(const std::exception&){rejected=true;}
    require(rejected,"invalid signed device identity was accepted");
    std::filesystem::remove_all(root);std::cout<<"native direct QUIC E2E IPv4 datagram/stream, reverse, replay and identity checks passed (no Relay running)\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::error_code ignored;std::filesystem::remove_all(root,ignored);return 1;}}
