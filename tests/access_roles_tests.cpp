#include "lanlink/relay/network_service.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <filesystem>
#include <iostream>
using namespace lanlink;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){const auto path=std::filesystem::temp_directory_path()/auth::random_access_code();try{{
    storage::RelayStore store(path);relay::NetworkService service(store);
    const auto owner=auth::random_nonce(),admin=auth::random_nonce(),limited=auth::random_nonce(),member=auth::random_nonce(),applicant=auth::random_nonce();
    const auto id=service.create_network(owner,{"Roles"},1).result.network_id;
    for(const auto& d:{owner,admin,limited,member,applicant}){service.record_authenticated_device(d,1);static_cast<void>(service.set_management_capable(d,true));}
    for(const auto& d:{admin,limited,member}){static_cast<void>(service.invite_member(owner,{id,d},1));check(service.join_network(d,{id},1).result.code==protocol::NetworkResultCode::success,"join");}
    protocol::ManagementRequest q;q.network_id=id;q.action=protocol::ManagementAction::replace_policy;q.policy.managed=true;
    q.policy.groups={"staff","guests"};q.policy.members={{admin,protocol::AccessRole::administrator,"staff"},{limited,protocol::AccessRole::restricted,"guests"},{member,protocol::AccessRole::member,"guests"}};
    q.policy.rules.push_back({{{},"staff"},{{},"guests"},true});q.policy.rules.push_back({{owner,{}},{},true});
    auto result=service.manage_network(owner,q,1);check(result.code==protocol::NetworkResultCode::success,"roles saved");
    check(protocol::policy_allows(result.policy,admin,limited) && !protocol::policy_allows(result.policy,limited,member),"group ACL");
    check(!protocol::policy_fanout_allows(result.policy,admin,limited) && protocol::policy_fanout_allows(result.policy,admin,member),"restricted fanout");
    protocol::ManagementRequest gateway;gateway.network_id=id;gateway.action=protocol::ManagementAction::request_gateway;gateway.policy.revision=result.policy.revision;
    check(service.manage_network(limited,gateway,1).code==protocol::NetworkResultCode::permission_denied,"restricted gateway request");
    check(service.join_network(applicant,{id},1).result.code==protocol::NetworkResultCode::pending_approval,"join request");
    protocol::ManagementRequest inspect;inspect.network_id=id;
    const auto pending=service.manage_network(admin,inspect,1);check(pending.pending_requests==std::vector<auth::DeviceId>{applicant},"admin can inspect persisted approvals");
    check(service.manage_network(member,inspect,1).pending_requests.empty(),"pending requests leaked to ordinary member");
    check(service.approve_member(admin,{id,applicant},1).result.code==protocol::NetworkResultCode::success,"admin approval");
    check(service.kick_member(admin,{id,owner},1).result.code==protocol::NetworkResultCode::permission_denied,"admin kicked owner");
    check(service.kick_member(admin,{id,admin},1).result.code==protocol::NetworkResultCode::permission_denied,"admin changed administrator membership");
    q.policy=result.policy;q.policy.members[1].role=protocol::AccessRole::administrator;
    check(service.manage_network(admin,q,1).code==protocol::NetworkResultCode::permission_denied,"admin granted a role");
    q.policy=result.policy;q.policy.rules.push_back({{limited,{}},{member,{}},false});
    result=service.manage_network(admin,q,1);check(result.code==protocol::NetworkResultCode::success,"admin edits data rules");
    check(service.leave_network(limited,{id},1).result.code==protocol::NetworkResultCode::success,"limited leave");
    check(protocol::policy_role(store.network_policy(id),limited)==protocol::AccessRole::restricted,"limited role lost on leave");
    static_cast<void>(service.invite_member(admin,{id,limited},1));check(service.join_network(limited,{id},1).result.code==protocol::NetworkResultCode::success,"limited rejoin");
    check(protocol::policy_role(store.network_policy(id),limited)==protocol::AccessRole::restricted,"rejoin escalated limited role");
    q.policy=store.network_policy(id);q.policy.members[0].role=protocol::AccessRole::member;
    check(service.manage_network(owner,q,1).code==protocol::NetworkResultCode::success,"owner demotes admin");
    check(service.kick_member(admin,{id,member},1).result.code==protocol::NetworkResultCode::permission_denied,"demoted administrator retained authority");
  }std::filesystem::remove(path);std::cout<<"roles, groups, admission authority, demotion and restricted rejoin passed\n";return 0;
}catch(const std::exception& e){std::filesystem::remove(path);std::cerr<<e.what()<<'\n';return 1;}}
