#include "lanlink/protocol/network_workflow.hpp"
#include "lanlink/protocol/join_target.hpp"
#include "lanlink/protocol/network_overview.hpp"
#include <iostream>
#include <stdexcept>
using namespace lanlink::protocol;
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){try{
    const auto legacy=create_network_command("Room","",false);require(legacy.command==LocalCommand::create,"legacy create path");
    const auto protected_room=create_network_command("Room","secure password",true);const auto policy=decode_access_request(protected_room.payload);
    require(policy.action==AccessAction::network_create_protected && policy.require_approval && policy.text=="Room","atomic protected creation command");
    const auto plain=join_network_command("01000000000000000000000000000000","");require(plain.command==LocalCommand::join,"legacy join path");
    NetworkId id{};id[0]=std::byte{1};const std::string secret(40,'a');
    const auto invite=join_network_command(make_invite_link(id,secret),"ignored password");
    require(decode_access_request(invite.payload).action==AccessAction::invite_redeem && decode_access_request(invite.payload).secret==secret,"clipboard link redeems scoped invitation");
    const auto password=join_network_command("01000000000000000000000000000000","secure password");
    require(decode_access_request(password.payload).action==AccessAction::password_join,"password join path");
    NetworkPeerState peers;peers.network_id=id;
    NetworkPeer online;online.device_id[0]=std::byte{2};online.signed_key=lanlink::auth::SignedDeviceKey{};
    peers.peers.push_back(online);NetworkPeer offline;offline.device_id[0]=std::byte{3};peers.peers.push_back(offline);
    PathDiagnostics paths;PathStatus direct;direct.kind=PathKind::direct_quic;direct.lifecycle=PathLifecycle::authenticated;
    paths.peers.push_back({id,online.device_id,0,direct});paths.peers.push_back({id,offline.device_id,0,direct});
    const auto overview=network_overview(peers,&paths,true);
    require(overview.total==3 && overview.online==2 && overview.direct==1,"overview counts current authenticated peers only");
    require(network_overview(peers,&paths,false).online==0,"disconnected overview has no live presence");
    std::cout<<"network workflows passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
