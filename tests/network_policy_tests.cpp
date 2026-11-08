#include "lanlink/relay/network_service.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace lanlink;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){const auto file=std::filesystem::temp_directory_path()/auth::random_access_code();try{{
    storage::RelayStore store(file);relay::NetworkService service(store);
    const auto owner=auth::random_nonce(),a=auth::random_nonce(),b=auth::random_nonce();
    const auto network=service.create_network(owner,{"Managed mesh"},1000).result.network_id;
    for(const auto& d:{a,b}){service.record_authenticated_device(d,1000);static_cast<void>(service.invite_member(owner,{network,d},1000));check(service.join_network(d,{network},1000).result.code==protocol::NetworkResultCode::success,"join fixture");}
    static_cast<void>(service.set_management_capable(owner,true));
    protocol::ManagementRequest q;q.action=protocol::ManagementAction::replace_policy;q.network_id=network;q.policy.managed=true;
    q.policy.rules.push_back({{owner,{}},{a,{}},true});
    std::vector<relay::RoutedPeerStateChange> changes;
    const auto reply=service.manage_network(owner,q,1000,&changes);
    check(reply.code==protocol::NetworkResultCode::success && reply.policy.revision==1 && changes.size()==3,"policy saved and announced");
    check(protocol::decode_management_result(protocol::encode_management_result(reply))==reply,"management codec");
    check(protocol::policy_allows(reply.policy,owner,a) && !protocol::policy_allows(reply.policy,a,b),"mesh allow list and default deny");
    check(service.manage_network(owner,q,1000).code==protocol::NetworkResultCode::conflict,"stale edit rejected");
    check(service.manage_network(a,q,1000).code==protocol::NetworkResultCode::permission_denied,"member cannot edit policy");
    const auto state=service.peer_state(owner,{network},1000).state;
    check(state && state->policy==reply.policy && protocol::decode_network_peer_state(protocol::encode_network_peer_state(*state))==state,"policy in authenticated peer state");
    check(store.network_policy(network)==reply.policy,"persistent policy");
    q.policy=reply.policy;q.policy.rules.push_back({{}, {}, false});
    check(!protocol::policy_allows(q.policy,owner,a),"deny wins");
    static_cast<void>(service.set_management_capable(a,true));
    q.policy=reply.policy;q.policy.gateways.push_back({a,false});
    check(service.manage_network(owner,q,1000).code==protocol::NetworkResultCode::permission_denied,"owner cannot fabricate a device gateway request");
    q.policy.gateways[0].approved=true;
    check(service.manage_network(owner,q,1000).code==protocol::NetworkResultCode::permission_denied,"gateway needs device consent");
    protocol::ManagementRequest enrollment;enrollment.action=protocol::ManagementAction::request_gateway;enrollment.network_id=network;enrollment.policy.revision=1;
    const auto pending=service.manage_network(a,enrollment,1000);
    check(pending.code==protocol::NetworkResultCode::success && pending.policy.gateways.size()==1 && !pending.policy.gateways[0].approved,"member requests pending gateway");
    q.policy=pending.policy;q.policy.gateways[0].approved=true;
    check(service.manage_network(a,q,1000).code==protocol::NetworkResultCode::permission_denied,"gateway cannot self approve");
    const auto approved=service.manage_network(owner,q,1000);
    check(approved.code==protocol::NetworkResultCode::success && approved.policy.gateways[0].approved,"owner approves gateway");
    q.policy=approved.policy;q.policy.routes.push_back({a,0xc0a8fb00U,24,{}});
    const auto routed=service.manage_network(owner,q,1000);
    check(routed.code==protocol::NetworkResultCode::success && routed.policy.routes.size()==1,"approved routed subnet");
    q.policy=routed.policy;q.policy.routes[0].address=0x0a400000U;
    check(service.manage_network(owner,q,1000).code==protocol::NetworkResultCode::conflict,"overlay conflict denied");
    auto invalid=routed.policy;invalid.routes[0].address=0;invalid.routes[0].prefix=0;
    bool bad=false;try{static_cast<void>(protocol::encode_network_policy(invalid));}catch(const std::exception&){bad=true;}check(bad,"default route rejected");
    q.policy=routed.policy;q.policy.routes.clear();q.policy.gateways.clear();
    check(service.manage_network(owner,q,1000).code==protocol::NetworkResultCode::success,"owner can revoke gateway");
  }{storage::RelayStore reopened(file);check(reopened.schema_version()==storage::current_schema_version,"schema migration");}
  std::filesystem::remove(file);std::cout<<"managed policy passed\n";return 0;
}catch(const std::exception& e){std::filesystem::remove(file);std::cerr<<e.what()<<'\n';return 1;}}
