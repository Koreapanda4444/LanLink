#include "lanlink/auth/handshake.hpp"
#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/p2p/peer_authorization.hpp"
#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/ipv4.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lanlink;
namespace {
constexpr std::array<const char*,3> labels{"windows","linux","macos"};
constexpr std::uint32_t subnet=0x0a782a00U;
constexpr std::uint64_t revision=0x1020304050607080ULL;
std::uint64_t epoch(unsigned slot){return 0x1122334455667700ULL+slot;}
protocol::NetworkId network_id(){protocol::NetworkId id{};id[0]=std::byte{0x49};id[15]=std::byte{0x80};return id;}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejected(F&& operation,const char* message){try{operation();}catch(const std::exception&){return;}throw std::runtime_error(message);}
std::vector<std::byte> read(const std::filesystem::path& path){
    const auto size=std::filesystem::file_size(path);require(size<=65536,"oversized interop fixture");
    std::vector<std::byte> data(static_cast<std::size_t>(size));std::ifstream input(path,std::ios::binary);
    input.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));require(static_cast<bool>(input),"cannot read fixture");return data;
}
void write(const std::filesystem::path& path,std::span<const std::byte> data){
    std::filesystem::create_directories(path.parent_path());auto temporary=path;temporary+=".tmp";
    std::ofstream output(temporary,std::ios::binary);output.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));output.close();require(static_cast<bool>(output),"cannot write fixture");std::filesystem::rename(temporary,path);
}
void marker(const std::filesystem::path& path){const std::byte ready{1};write(path,std::span(&ready,1));}
std::vector<std::byte> frame(protocol::MessageType type,std::vector<std::byte> bytes){return protocol::encode_frame({type,0x8090a0b0U,std::move(bytes)});}
std::vector<std::byte> payload(const std::filesystem::path& path,protocol::MessageType type){
    const auto bytes=read(path);const auto decoded=protocol::decode_frame(bytes);
    require(decoded.complete()&&decoded.consumed==bytes.size()&&decoded.frame->type==type&&decoded.frame->request_id==0x8090a0b0U,"foreign frame differs in type, version or request ID");
    require(protocol::encode_frame(*decoded.frame)==bytes,"foreign frame is not canonical");return decoded.frame->payload;
}
std::vector<std::byte> packet(unsigned sender,unsigned receiver,std::size_t size){
    std::vector<std::byte> data(size);data[0]=std::byte{0x45};data[2]=std::byte((size>>8)&255);data[3]=std::byte(size&255);data[6]=std::byte{0x40};data[8]=std::byte{64};data[9]=std::byte{17};
    for(unsigned i=0;i<4;++i){data[12+i]=std::byte(((subnet+sender+1)>>((3-i)*8))&255);data[16+i]=std::byte(((subnet+receiver+1)>>((3-i)*8))&255);}
    data[20]=std::byte{0x40};data[21]=std::byte{0x49};data[22]=std::byte{0x50};data[23]=std::byte{0x49};data[24]=std::byte(((size-20)>>8)&255);data[25]=std::byte((size-20)&255);
    for(std::size_t i=28;i<size;++i)data[i]=std::byte((i*13+sender*17+receiver*29)&255);
    protocol::update_ipv4_checksum(data);return data;
}
p2p::PeerBinding binding(const protocol::NetworkPeerState& a,const protocol::NetworkPeerState& b,unsigned first,unsigned second){
    return {network_id(),a.key_epoch,0x100000000ULL+first+1,0x200000000ULL+second+1,
        a.owner_device_id,b.owner_device_id,a.peers.front().signed_key->encryption_public_key,b.peers.front().signed_key->encryption_public_key};
}
const char* native_platform(){
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}
int peer(const std::filesystem::path& root,unsigned own){
    const auto identity=auth::DeviceIdentity::load_or_create(root/"private/device.identity");
    const auto token=auth::AuthToken::from_secret("lanlink-cross-platform-isolated-interop-auth-token");
    auth::ClientHandshake client(identity,token);auth::ServerHandshake server(token,1,auth::ServerHandshake::Clock::now()+std::chrono::minutes{1});
    require(client.handle_result(server.handle_proof(client.handle_challenge(server.handle_hello(client.begin(1))))),"fixture authentication failed");
    const auto own_key=*client.signed_device_key();
    protocol::NetworkPeerState state{network_id(),revision,subnet,24,subnet+own+1,
        {{identity.device_id(),subnet+own+1,own_key}},identity.device_id(),epoch(own)};
    protocol::NetworkPolicy policy;policy.managed=true;policy.revision=revision;policy.groups={"interop"};policy.members={{identity.device_id(),protocol::AccessRole::administrator,"interop"}};policy.rules.push_back({{},{},true});state.policy=policy;
    auto advertised=state;advertised.own_address=subnet+254;
    write(root/"public"/(std::string(labels[own])+".state"),frame(protocol::MessageType::network_peer_state_result,protocol::encode_network_peer_state(advertised)));marker(root/"public.ready");
    const auto group_key=auth::NetworkKey::random();std::array<protocol::NetworkPeerState,3> records;bool published=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes{30};
    while(std::chrono::steady_clock::now()<deadline){
        if(std::filesystem::exists(root/"stop"))return 1;
        if(!published&&std::filesystem::exists(root/"keys.ready")){
            for(unsigned i=0;i<labels.size();++i){
                records[i]=protocol::decode_network_peer_state(payload(root/"keys"/(std::string(labels[i])+".state"),protocol::MessageType::network_peer_state_result));
                const auto& record=records[i];require(record.network_id==network_id()&&record.revision==revision&&record.key_epoch==epoch(i)&&record.own_address==subnet+254&&record.peers.size()==1&&record.peers.front().ipv4_address==subnet+i+1&&record.peers.front().signed_key&&record.policy&&record.policy->managed&&record.policy->revision==revision,"foreign peer state differs");
                require(auth::verify_signed_device_key(*record.peers.front().signed_key,record.owner_device_id)&&record.peers.front().device_id==record.owner_device_id,"foreign signed identity is invalid");
                require(protocol::policy_group(*record.policy,record.owner_device_id)=="interop"&&protocol::policy_role(*record.policy,record.owner_device_id)==protocol::AccessRole::administrator,"foreign group or role differs");
                records[i].own_address=record.peers.front().ipv4_address;
            }
            require(records[own]==state,"public key changed during exchange");
            auto peers=state;peers.peers.clear();for(const auto& record:records)if(record.owner_device_id!=identity.device_id())peers.peers.push_back(record.peers.front());
            auth::NetworkPacketCipher cipher(network_id(),epoch(own),group_key,identity.device_id(),own_key,state.own_address);cipher.update_peers(peers);
            for(unsigned i=0;i<labels.size();++i)if(i!=own){
                const auto prefix=std::string(labels[own])+"-to-"+labels[i];
                const auto envelope=auth::seal_network_key(identity,client.encryption_key(),*records[i].peers.front().signed_key,records[i].owner_device_id,network_id(),epoch(own),group_key);
                write(root/"outgoing"/(prefix+".key"),frame(protocol::MessageType::network_key_envelope,protocol::encode_network_key_envelope(envelope)));
                write(root/"outgoing"/(prefix+".packet"),frame(protocol::MessageType::network_packet_send,protocol::encode_encrypted_network_packet(cipher.encrypt(packet(own,i,1400)))));
                write(root/"outgoing"/(prefix+".next"),frame(protocol::MessageType::network_packet_send,protocol::encode_encrypted_network_packet(cipher.encrypt(packet(own,i,64)))));
                const auto context=binding(state,records[i],own,i);auto proof=p2p::encode_binding(context);
                const auto signature=p2p::sign_peer_binding(identity,context,own_key.client_nonce,records[i].peers.front().signed_key->client_nonce);proof.insert(proof.end(),signature.begin(),signature.end());write(root/"outgoing"/(prefix+".proof"),proof);
            }
            published=true;marker(root/"outgoing.ready");
        }
        if(published&&std::filesystem::exists(root/"messages.ready")){
            for(unsigned i=0;i<labels.size();++i)if(i!=own){
                const auto prefix=std::string(labels[i])+"-to-"+labels[own];const auto& sender=records[i];const auto& sender_key=*sender.peers.front().signed_key;
                const auto encoded=payload(root/"incoming"/(prefix+".key"),protocol::MessageType::network_key_envelope);const auto envelope=protocol::decode_network_key_envelope(encoded);
                require(protocol::encode_network_key_envelope(envelope)==encoded&&envelope.epoch==sender.key_epoch&&auth::verify_network_key_envelope(envelope,sender_key.identity_public_key),"foreign key envelope failed verification");
                const auto opened=auth::open_network_key(client.encryption_key(),envelope,sender_key,identity.device_id());
                auto changed=envelope;changed.signature[0]^=std::byte{1};rejected([&]{static_cast<void>(auth::open_network_key(client.encryption_key(),changed,sender_key,identity.device_id()));},"altered foreign envelope was accepted");
                rejected([&]{static_cast<void>(auth::open_network_key(client.encryption_key(),envelope,sender_key,sender.owner_device_id));},"foreign envelope accepted the wrong recipient");
                auto peers=sender;peers.own_address=state.own_address;peers.peers.clear();for(const auto& record:records)if(record.owner_device_id!=identity.device_id())peers.peers.push_back(record.peers.front());
                auth::NetworkPacketCipher cipher(network_id(),sender.key_epoch,opened,identity.device_id(),own_key,state.own_address);cipher.update_peers(peers);
                const auto encrypted=protocol::decode_encrypted_network_packet(payload(root/"incoming"/(prefix+".packet"),protocol::MessageType::network_packet_send));
                auto tampered=encrypted;tampered.tag[0]^=std::byte{1};rejected([&]{static_cast<void>(cipher.decrypt(tampered,sender.owner_device_id));},"altered foreign packet was accepted");
                tampered=encrypted;++tampered.key_epoch;rejected([&]{static_cast<void>(cipher.decrypt(tampered,sender.owner_device_id));},"foreign packet epoch was ignored");
                require(cipher.decrypt(encrypted,sender.owner_device_id)==packet(i,own,1400),"foreign packet plaintext or MTU boundary differs");
                rejected([&]{static_cast<void>(cipher.decrypt(encrypted,sender.owner_device_id));},"foreign packet replay was accepted");
                const auto expected=binding(sender,state,i,own);const auto proof=read(root/"incoming"/(prefix+".proof"));const auto binding_size=p2p::encode_binding(expected).size();require(proof.size()==binding_size+auth::signature_size,"foreign Direct proof size differs");
                require(p2p::decode_binding(std::span(proof).first(binding_size))==expected,"foreign Direct binding differs");auth::Signature signature{};std::copy_n(proof.begin()+static_cast<std::ptrdiff_t>(binding_size),signature.size(),signature.begin());
                require(p2p::verify_peer_binding(sender_key,expected,sender_key.client_nonce,own_key.client_nonce,signature),"foreign Direct proof failed");auto invalid=expected;++invalid.key_epoch;
                require(!p2p::verify_peer_binding(sender_key,invalid,sender_key.client_nonce,own_key.client_nonce,signature),"altered foreign Direct proof was accepted");
                session::NetworkSnapshot snapshot{{session::PacketIdentitySnapshot{identity.device_id(),own_key}},{{peers,{sender.key_epoch,opened}}}};
                require(p2p::peer_binding_authorized(expected.reversed(),sender_key,state.own_address,sender.own_address,snapshot),"foreign Direct binding was not authorized");
                peers.policy->rules.clear();++peers.policy->revision;cipher.update_peers(peers);snapshot.networks.front().peers=peers;
                const auto next=protocol::decode_encrypted_network_packet(payload(root/"incoming"/(prefix+".next"),protocol::MessageType::network_packet_send));
                rejected([&]{static_cast<void>(cipher.decrypt(next,sender.owner_device_id));},"foreign sender bypassed default deny");
                require(!p2p::peer_binding_authorized(expected.reversed(),sender_key,state.own_address,sender.own_address,snapshot),"foreign Direct binding bypassed default deny");
            }
            std::cout<<native_platform()<<": two foreign peers, X25519 key envelopes, AES-GCM IPv4, MTU, Direct proofs, replay/tamper/epoch/ACL rejection passed\n"<<std::flush;marker(root/"verified.ready");return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    throw std::runtime_error("cross-platform exchange timed out");
}
}
int main(int argc,char* argv[]){
    std::filesystem::path root;
    try{
        require(argc==3||(argc==4&&std::string_view(argv[3])=="--local-fixture"),"usage: interop-peer DIRECTORY PLATFORM [--local-fixture]");root=argv[1];unsigned slot=3;
        for(unsigned i=0;i<labels.size();++i)if(std::string_view(argv[2])==labels[i])slot=i;
        require(slot<3,"unknown interop platform");
        require(argc==4||std::string_view(argv[2])==native_platform(),"CI peer must run on its native OS");return peer(root,slot);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';if(!root.empty())try{std::ofstream(root/"failed.txt")<<error.what();}catch(...){}return 1;}
}
