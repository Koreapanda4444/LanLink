#include "lanlink/relay/network_service.hpp"
#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/auth/access_secret.hpp"
#include "lanlink/p2p/peer_authorization.hpp"
#include <filesystem>
#include <iostream>
using namespace lanlink;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
auth::SignedDeviceKey key(const auth::DeviceIdentity& i){auth::SignedDeviceKey k;k.identity_public_key=i.public_key();k.encryption_public_key=auth::DeviceEncryptionKey::generate().public_key();k.client_nonce=auth::random_nonce();k.server_nonce=auth::random_nonce();k.signature=i.sign(auth::make_auth_transcript(k.identity_public_key,k.encryption_public_key,k.client_nonce,k.server_nonce));return k;}
std::vector<std::byte> ipv4(std::uint32_t from,std::uint32_t to){std::vector<std::byte> b(28);b[0]=std::byte{0x45};b[3]=std::byte{28};b[8]=std::byte{64};b[9]=std::byte{17};for(unsigned i=0;i<4;++i){b[12+i]=static_cast<std::byte>(from>>(24-i*8));b[16+i]=static_cast<std::byte>(to>>(24-i*8));}return b;}
}
int main(){const auto root=std::filesystem::temp_directory_path()/auth::random_access_code();try{{
    storage::RelayStore store(root/"relay.db");relay::NetworkService service(store);
    const auto hub=auth::DeviceIdentity::load_or_create(root/"hub"),a=auth::DeviceIdentity::load_or_create(root/"a"),b=auth::DeviceIdentity::load_or_create(root/"b");
    const auto hk=key(hub),ak=key(a),bk=key(b);
    for(const auto& [id,k]:{std::pair{hub.device_id(),hk},std::pair{a.device_id(),ak},std::pair{b.device_id(),bk}}) {
        static_cast<void>(service.publish_device_key(id,auth::random_nonce(),k,1000));static_cast<void>(service.set_management_capable(id,true));
    }
    const auto network=service.create_network(hub.device_id(),{"Hub and spoke"},1000).result.network_id;
    for(const auto& d:{a.device_id(),b.device_id()}){static_cast<void>(service.invite_member(hub.device_id(),{network,d},1000));check(service.join_network(d,{network},1000).result.code==protocol::NetworkResultCode::success,"join");}
    protocol::ManagementRequest q;q.network_id=network;q.action=protocol::ManagementAction::replace_policy;q.policy.managed=true;
    q.policy.topology=protocol::NetworkTopology::hub_spoke;q.policy.hubs={hub.device_id()};q.policy.rules.push_back({{}, {}, true});
    const auto reply=service.manage_network(hub.device_id(),q,1000);check(reply.code==protocol::NetworkResultCode::success,"hub policy");
    auto ap=*service.peer_state(a.device_id(),{network},1000).state,bp=*service.peer_state(b.device_id(),{network},1000).state,hp=*service.peer_state(hub.device_id(),{network},1000).state;
    const auto secret=auth::NetworkKey::random();
    auth::NetworkPacketCipher ac(network,ap.key_epoch,secret,a.device_id(),ak,ap.own_address),bc(network,bp.key_epoch,secret,b.device_id(),bk,bp.own_address),hc(network,hp.key_epoch,secret,hub.device_id(),hk,hp.own_address);
    ac.update_peers(ap);bc.update_peers(bp);hc.update_peers(hp);
    const auto forbidden=ac.encrypt(ipv4(ap.own_address,bp.own_address));
    check(service.route_encrypted_packet(a.device_id(),forbidden).empty(),"relay allowed spoke to spoke");
    bool rejected=false;try{static_cast<void>(bc.decrypt(forbidden,a.device_id()));}catch(const std::exception&){rejected=true;}
    check(rejected,"cipher accepted a disallowed spoke");
    const auto permitted=ac.encrypt(ipv4(ap.own_address,hp.own_address));
    check(service.route_encrypted_packet(a.device_id(),permitted).size()==1 && hc.decrypt(permitted,a.device_id())==ipv4(ap.own_address,hp.own_address),"spoke to hub round trip");
    p2p::CandidateExchange x;x.network_id=network;x.sender=a.device_id();x.recipient=b.device_id();x.sender_session=ak.encryption_public_key;x.recipient_session=bk.encryption_public_key;x.key_epoch=ap.key_epoch;
    check(!service.authorize_peer_exchange(a.device_id(),x),"candidate bypass");
    p2p::PeerBinding binding;binding.network_id=network;binding.key_epoch=ap.key_epoch;binding.local_generation=binding.peer_generation=1;binding.local_device=a.device_id();binding.peer_device=b.device_id();binding.local_session=ak.encryption_public_key;binding.peer_session=bk.encryption_public_key;
    session::NetworkSnapshot snapshot{{{a.device_id(),ak}},{{ap,{ap.key_epoch,secret}}}};
    check(!p2p::peer_binding_authorized(binding,bk,ap.own_address,bp.own_address,snapshot),"direct authorization bypass");
    const auto fanout=service.route_encrypted_packet(a.device_id(),ac.encrypt(ipv4(ap.own_address,0xffffffffU)));
    check(fanout.size()==1 && fanout.front().recipient_device_id==hub.device_id(),"broadcast leaked between spokes");
    auto old=reply.policy;q.policy=old;q.policy.hubs={a.device_id()};const auto changed=service.manage_network(hub.device_id(),q,1000);
    check(changed.code==protocol::NetworkResultCode::success && service.route_encrypted_packet(a.device_id(),permitted).empty(),"old epoch accepted after topology change");
    check(service.kick_member(hub.device_id(),{network,a.device_id()},1000).result.code==protocol::NetworkResultCode::success,"kick hub");
    check(service.route_encrypted_packet(a.device_id(),permitted).empty(),"removed hub retained traffic");
  }std::filesystem::remove_all(root);std::cout<<"managed relay, cipher, direct binding, broadcast and epoch restrictions passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(root);return 1;}}
