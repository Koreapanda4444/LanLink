#include "lanlink/p2p/peer_authorization.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
namespace lanlink::p2p {
namespace {
std::vector<std::byte> transcript(const PeerBinding& b,const auth::Nonce& sender,const auth::Nonce& receiver) {
    protocol::BinaryWriter w;w.put<std::uint32_t>(0x4c4c4431);w.append(encode_binding(b));w.append(sender);w.append(receiver);return std::move(w).finish();
}
bool fresh(const auth::Nonce& a,const auth::Nonce& b){return a!=auth::Nonce{} && b!=auth::Nonce{} && a!=b;}
}
bool peer_binding_authorized(const PeerBinding& b,const auth::SignedDeviceKey& key,std::uint32_t local,std::uint32_t peer,
    const session::NetworkSnapshot& s) {
    if(!s.identity || s.identity->device_id!=b.local_device || !b.local_generation || !b.peer_generation ||
        s.identity->signed_key.encryption_public_key!=b.local_session || !auth::verify_signed_device_key(s.identity->signed_key,b.local_device) ||
        key.encryption_public_key!=b.peer_session || !auth::verify_signed_device_key(key,b.peer_device))return false;
    for(const auto& n:s.networks)if(n.peers.network_id==b.network_id) {
        if(n.key.epoch!=b.key_epoch || n.peers.key_epoch!=b.key_epoch || n.peers.own_address!=local)return false;
        return std::any_of(n.peers.peers.begin(),n.peers.peers.end(),[&](const auto& p){
            return p.device_id==b.peer_device && p.ipv4_address==peer && p.signed_key && *p.signed_key==key;});
    }
    return false;
}
auth::Signature sign_peer_binding(const auth::DeviceIdentity& i,const PeerBinding& b,const auth::Nonce& a,const auth::Nonce& c) {
    if(i.device_id()!=b.local_device || !fresh(a,c))throw std::invalid_argument("invalid peer proof context");
    return i.sign(transcript(b,a,c));
}
bool verify_peer_binding(const auth::SignedDeviceKey& key,const PeerBinding& b,const auth::Nonce& a,const auth::Nonce& c,const auth::Signature& signature) {
    return fresh(a,c) && b.local_session==key.encryption_public_key && auth::verify_signed_device_key(key,b.local_device) &&
        auth::verify_signature(key.identity_public_key,transcript(b,a,c),signature);
}
}
