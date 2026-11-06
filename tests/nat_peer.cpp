#include "lanlink/p2p/direct_quic.hpp"
#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/quic.hpp"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

using namespace lanlink;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void save(const std::filesystem::path& file, std::span<const std::byte> data) {
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    require(static_cast<bool>(output), "cannot save NAT fixture");
}
std::vector<std::byte> load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(static_cast<bool>(input), "cannot load NAT fixture");
    const std::string data{std::istreambuf_iterator<char>(input), {}};
    return {reinterpret_cast<const std::byte*>(data.data()), reinterpret_cast<const std::byte*>(data.data()+data.size())};
}
auth::SignedDeviceKey key(const auth::DeviceIdentity& identity) {
    auth::SignedDeviceKey result;
    result.identity_public_key=identity.public_key();
    result.encryption_public_key=auth::DeviceEncryptionKey::generate().public_key();
    result.client_nonce=auth::random_nonce(); result.server_nonce=auth::random_nonce();
    result.signature=identity.sign(auth::make_auth_transcript(result.identity_public_key,result.encryption_public_key,result.client_nonce,result.server_nonce));
    return result;
}
template<class F> void wait(const char* operation,F ready) {
    const auto end=std::chrono::steady_clock::now()+15s;
    while(!ready()) { require(std::chrono::steady_clock::now()<end,operation); std::this_thread::sleep_for(20ms); }
}
std::vector<std::byte> packet(std::uint32_t source,std::uint32_t destination) {
    std::vector<std::byte> data(128,std::byte{0x71});data[0]=std::byte{0x45};data[2]=std::byte{0};data[3]=std::byte{128};
    data[8]=std::byte{64};data[9]=std::byte{17};
    for(unsigned i=0;i<4;++i) { data[12+i]=static_cast<std::byte>(source>>((3-i)*8));data[16+i]=static_cast<std::byte>(destination>>((3-i)*8)); }
    return data;
}
}
int main(int argc,char** argv) { try {
    std::signal(SIGPIPE, SIG_IGN);
    require(argc==3 || argc==6,"usage: nat-peer fixture prepare | fixture a|b remote-port local-port direct|blocked");
    const std::filesystem::path root=argv[1];
    if(std::string_view(argv[2])=="prepare") {
        const auto a=auth::DeviceIdentity::load_or_create(root/"a.identity"),b=auth::DeviceIdentity::load_or_create(root/"b.identity");
        const auto ak=key(a),bk=key(b);protocol::NetworkId id{};id[0]=std::byte{1};
        const protocol::NetworkPeerState left{id,1,0x0a400000,24,0x0a400001,{{b.device_id(),0x0a400002,bk}},a.device_id(),1};
        const protocol::NetworkPeerState right{id,1,0x0a400000,24,0x0a400002,{{a.device_id(),0x0a400001,ak}},a.device_id(),1};
        save(root/"a.state",protocol::encode_network_peer_state(left));save(root/"b.state",protocol::encode_network_peer_state(right));
        save(root/"network.key",auth::NetworkKey::random().bytes());return 0;
    }
    require(argc==6,"missing NAT peer arguments");
    const bool left=std::string_view(argv[2])=="a";const std::string name=left?"a":"b";
    const auto identity=auth::DeviceIdentity::load_or_create(root/(name+".identity"));
    const auto state=protocol::decode_network_peer_state(load(root/(name+".state")));
    const auto other=protocol::decode_network_peer_state(load(root/(left?"b.state":"a.state")));
    const auto own_key=*other.peers.front().signed_key,peer_key=*state.peers.front().signed_key;
    const auto bytes=load(root/"network.key");require(bytes.size()==32,"invalid fixture network key");
    std::array<std::byte,32> raw{};std::copy(bytes.begin(),bytes.end(),raw.begin());const auth::NetworkKey network_key(raw);
    p2p::PeerBinding binding;binding.network_id=state.network_id;binding.key_epoch=1;
    binding.local_generation=binding.peer_generation=1;binding.local_device=identity.device_id();binding.peer_device=state.peers.front().device_id;
    binding.local_session=own_key.encryption_public_key;binding.peer_session=peer_key.encryption_public_key;
    const auto port=[](const char* text){const auto value=std::stoul(text);require(value>0 && value<=65535,"invalid fixture port");return static_cast<std::uint16_t>(value);};
    const bool loopback=std::string_view(argv[5])=="loopback";
    const p2p::Endpoint remote{loopback?0x7f000001U:(left?0xcb007102U:0xcb007101U),port(argv[3])};
    auto socket=std::make_unique<p2p::UdpSocket>(p2p::Endpoint{0,port(argv[4])});
    p2p::PunchSession punch(identity,peer_key,binding,{{remote,p2p::CandidateKind::server_reflexive,100}});
    const auto selected=p2p::run_punch(*socket,punch);socket.reset();
    if(std::string_view(argv[5])=="blocked") {
        require(!selected,"blocked UDP unexpectedly selected a Direct candidate");
        core::Logger logger(root/(name+"-fallback.log"),"nat",core::LogLevel::warning,1024*1024,2);
        transport::QuicClientOptions options;options.relay_host="203.0.113.254";options.port=4433;
        options.identity_file=root/(name+".identity");options.auth_token_file=root/"auth.token";
        options.handshake_timeout=1s;options.authentication_timeout=2s;
        transport::QuicRelayClient client(options,logger);
        std::exception_ptr error;
        std::jthread worker([&](std::stop_token stop){try{client.run(stop);}catch(...){error=std::current_exception();}});
        wait("NAT TLS authentication timed out",[&]{return client.authenticated() && client.using_tcp_fallback();});
        require(client.list_networks().code==protocol::NetworkResultCode::success,"TLS fallback control failed across NAT");
        worker.request_stop();client.stop();worker.join();if(error)std::rethrow_exception(error);
        std::cout<<name<<": kernel UDP block -> authenticated TLS fallback passed\n";return 0;
    }
    require(selected==remote,"kernel NAT punching did not validate the public endpoint");
    auth::NetworkPacketCipher cipher(state.network_id,1,network_key,identity.device_id(),own_key,state.own_address);cipher.update_peers(state);
    std::atomic<unsigned> received=0;
    p2p::DirectQuic peer(identity,{binding,peer_key,state.own_address,other.own_address,port(argv[4])},[&](const auto& delivery){
        require(cipher.decrypt(delivery.packet,delivery.sender_device_id)==packet(other.own_address,state.own_address),"NAT plaintext changed");++received;
    });
    peer.synchronize({{{identity.device_id(),own_key}},{{state,{1,network_key}}}});
    peer.start();
    // Both punched UDP sockets have now been replaced by their QUIC endpoint.
    // Do not let the faster certificate initialization send into a port whose
    // listener has not started: an ICMP refusal can end the single test dial.
    save(root/(name+".direct-ready"),{});
    wait("NAT peer listener readiness timed out",[&]{return std::filesystem::exists(root/(left?"b.direct-ready":"a.direct-ready"));});
    peer.connect(remote);wait("NAT Direct authentication timed out",[&]{return peer.authenticated();});
    const auto plaintext=packet(state.own_address,other.own_address);
    for(unsigned index=0;index<30;++index) {
        const auto encrypted=cipher.encrypt(plaintext);require(encrypted.ciphertext!=plaintext,"plaintext left NAT peer");
        require(peer.send(encrypted)==transport::PacketSendResult::accepted,"NAT Direct send failed");std::this_thread::sleep_for(30ms);
    }
    wait("NAT encrypted packet receive timed out",[&]{return received.load()>0;});peer.stop();
    std::cout<<name<<(loopback?": loopback fixture":": kernel SNAT/DNAT")<<", signed punch and encrypted Direct QUIC passed\n";return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; } }
