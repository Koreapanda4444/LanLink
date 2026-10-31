#include "lanlink/relay/network_service.hpp"
#include "lanlink/transport/packet_bridge.hpp"
#include "lanlink/network/routed_network.hpp"
#include "lanlink/auth/access_secret.hpp"
#include "lanlink/protocol/ipv4.hpp"
#include <filesystem>
#include <iostream>
using namespace lanlink;
namespace {
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
auth::SignedDeviceKey key(const auth::DeviceIdentity& i){auth::SignedDeviceKey k;k.identity_public_key=i.public_key();k.encryption_public_key=auth::DeviceEncryptionKey::generate().public_key();k.client_nonce=auth::random_nonce();k.server_nonce=auth::random_nonce();k.signature=i.sign(auth::make_auth_transcript(k.identity_public_key,k.encryption_public_key,k.client_nonce,k.server_nonce));return k;}
std::vector<std::byte> ipv4(std::uint32_t from,std::uint32_t to,std::size_t size=60){std::vector<std::byte> b(size);b[0]=std::byte{0x45};b[2]=static_cast<std::byte>(size>>8);b[3]=static_cast<std::byte>(size);b[8]=std::byte{64};b[9]=std::byte{17};for(unsigned i=0;i<4;++i){b[12+i]=static_cast<std::byte>(from>>(24-i*8));b[16+i]=static_cast<std::byte>(to>>(24-i*8));}protocol::update_ipv4_checksum(b);return b;}
}
int main(){const auto root=std::filesystem::temp_directory_path()/auth::random_access_code();try{{
    storage::RelayStore store(root/"db");relay::NetworkService service(store);
    const auto a=auth::DeviceIdentity::load_or_create(root/"a"),g=auth::DeviceIdentity::load_or_create(root/"gateway");const auto ak=key(a),gk=key(g);
    for(const auto& [id,k]:{std::pair{a.device_id(),ak},std::pair{g.device_id(),gk}}){static_cast<void>(service.publish_device_key(id,auth::random_nonce(),k,1));static_cast<void>(service.set_management_capable(id,true));}
    const auto id=service.create_network(a.device_id(),{"Routed"},1).result.network_id;
    static_cast<void>(service.invite_member(a.device_id(),{id,g.device_id()},1));check(service.join_network(g.device_id(),{id},1).result.code==protocol::NetworkResultCode::success,"join gateway");
    protocol::ManagementRequest q;q.network_id=id;q.action=protocol::ManagementAction::replace_policy;q.policy.managed=true;q.policy.rules.push_back({{}, {}, true});
    q.policy=service.manage_network(a.device_id(),q,1).policy;
    protocol::ManagementRequest request;request.network_id=id;request.action=protocol::ManagementAction::request_gateway;request.policy.revision=q.policy.revision;
    q.policy=service.manage_network(g.device_id(),request,1).policy;q.policy.gateways[0].approved=true;q.policy.routes.push_back({g.device_id(),0xc0a8fb00U,24,{}});
    check(service.manage_network(a.device_id(),q,1).code==protocol::NetworkResultCode::success,"approve route");
    const auto ap=*service.peer_state(a.device_id(),{id},1).state,gp=*service.peer_state(g.device_id(),{id},1).state;
    const auto secret=auth::NetworkKey::random();const session::NetworkSnapshot as{{{a.device_id(),ak}},{{ap,{ap.key_epoch,secret}}}},gs{{{g.device_id(),gk}},{{gp,{gp.key_epoch,secret}}}};
    check(network::routing_configuration(as,false,false).routes.empty(),"local route consent is off by default");
    const auto plan=network::routing_configuration(as,true,false),forward=network::routing_configuration(gs,false,true);
    check(plan.routes.size()==1 && forward.forwards.size()==1 && forward.forwards[0].member_address==ap.own_address,"scoped OS route and forwarding plan");
    std::vector<std::vector<std::byte>> lan,client;std::vector<protocol::EncryptedNetworkPacket> wire;
    transport::VirtualPacketBridge* target=nullptr;
    const auto send=[&](const auth::DeviceId& sender,const protocol::EncryptedNetworkPacket& packet){wire.push_back(packet);for(const auto& d:service.route_encrypted_packet(sender,packet))target->inbound(d.delivery);};
    transport::VirtualPacketBridge ab([&](const auto& p){send(a.device_id(),p);},[&](auto b){client.emplace_back(b.begin(),b.end());});
    transport::VirtualPacketBridge gb([&](const auto& p){send(g.device_id(),p);},[&](auto b){lan.emplace_back(b.begin(),b.end());});
    ab.synchronize(as.identity,as.networks);gb.synchronize(gs.identity,gs.networks);ab.set_mtu(1400);gb.set_mtu(1400);
    ab.set_routing_options(true,false);gb.set_routing_options(false,true);
    const auto outbound=ipv4(ap.own_address,0xc0a8fb03U);target=&gb;ab.outbound(outbound);
    check(lan.size()==1 && lan.back()==outbound && wire.back().ciphertext!=outbound && wire.back().destination_ipv4==gp.own_address,"encrypted gateway request");
    const auto reply=ipv4(0xc0a8fb03U,ap.own_address);target=&ab;gb.outbound(reply);
    check(client.size()==1 && client.back()==reply,"gateway return packet");
    auto boundary=ipv4(ap.own_address,0xc0a8fb03U,1400);boundary[6]=std::byte{0x40};protocol::update_ipv4_checksum(boundary);
    target=&gb;ab.outbound(boundary);
    check(lan.size()==2 && lan.back()==boundary && wire.back().ciphertext.size()==1420,"configured MTU with DF must fit the routed frame");
    auto boundary_reply=ipv4(0xc0a8fb03U,ap.own_address,1400);boundary_reply[6]=std::byte{0x40};protocol::update_ipv4_checksum(boundary_reply);
    target=&ab;gb.outbound(boundary_reply);
    check(client.size()==2 && client.back()==boundary_reply,"DF return packet at the adapter MTU was dropped");
    const auto before_fragments=lan.size();
    target=&gb;ab.outbound(ipv4(ap.own_address,0xc0a8fb03U,3000));check(lan.size()==before_fragments+3,"inner MTU fragmentation");
    for(std::size_t i=before_fragments;i<lan.size();++i)check(lan[i].size()<=1400 && protocol::inspect_ipv4(lan[i]).source==ap.own_address,"invalid routed fragment");
    gb.clear();gb.synchronize(gs.identity,gs.networks);
    auth::NetworkPacketCipher attacker(id,ap.key_epoch,secret,a.device_id(),ak,ap.own_address);attacker.update_peers(ap);
    const auto attempt=[&](const auto& inner){const auto outer=network::encapsulate_routed_ipv4(inner,ap.own_address,gp.own_address);gb.inbound({a.device_id(),attacker.encrypt(outer)});};
    const auto before=lan.size();attempt(ipv4(ap.own_address+9,0xc0a8fb03U));attempt(ipv4(ap.own_address,0x08080808U));
    check(lan.size()==before,"spoofed source or unauthorized subnet injected");
    auto nested=outbound;nested[9]=std::byte{4};protocol::update_ipv4_checksum(nested);attempt(nested);
    auto ttl=outbound;ttl[8]=std::byte{1};protocol::update_ipv4_checksum(ttl);attempt(ttl);
    auto redirect=outbound;redirect[9]=std::byte{1};redirect[20]=std::byte{5};protocol::update_ipv4_checksum(redirect);attempt(redirect);
    attempt(ipv4(ap.own_address,0xc0a8fbffU));
    check(lan.size()==before,"nested tunnel, expired TTL, ICMP redirect or broadcast forwarded");
    gb.set_routing_options(false,false);attempt(outbound);check(lan.size()==before,"forwarding consent bypass");
    ab.set_routing_options(false,false);const auto count=wire.size();ab.outbound(outbound);check(wire.size()==count,"client route consent bypass");
    auto disconnected=as;disconnected.networks.clear();check(network::routing_configuration(disconnected,true,true).routes.empty(),"disconnect keeps route");
    q.policy=store.network_policy(id);q.policy.routes.clear();q.policy.gateways.clear();check(service.manage_network(a.device_id(),q,1).code==protocol::NetworkResultCode::success,"withdraw route");
    check(service.route_encrypted_packet(a.device_id(),wire.front()).empty(),"old gateway epoch accepted");
    request.policy.revision=store.network_policy(id).revision;const auto new_request=service.manage_network(g.device_id(),request,1);
    q.policy=new_request.policy;q.policy.gateways[0].approved=true;q.policy.routes.push_back({g.device_id(),0xc0a8fb00U,24,{}});
    check(service.manage_network(a.device_id(),q,1).code==protocol::NetworkResultCode::success,"gateway approval fixture");
    check(service.kick_member(a.device_id(),{id,g.device_id()},1).result.code==protocol::NetworkResultCode::success,"gateway kick");
    check(store.network_policy(id).gateways.empty() && store.network_policy(id).routes.empty(),"removed gateway kept routes");
  }std::filesystem::remove_all(root);std::cout<<"gateway encrypted round trip, routes, consent, spoofing and MTU passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(root);return 1;}}
