#include "lanlink/relay/network_service.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <filesystem>
#include <iostream>
using namespace lanlink;
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){const auto file=std::filesystem::temp_directory_path()/auth::random_access_code();try{protocol::NetworkId id{};const auto owner=auth::random_nonce();{
    storage::RelayStore store(file);relay::NetworkService service(store);
    id=service.create_network(owner,{"secret-network-name-must-not-be-in-audit"},1).result.network_id;static_cast<void>(service.set_management_capable(owner,true));
    protocol::ManagementRequest q;q.network_id=id;q.action=protocol::ManagementAction::replace_policy;q.policy.managed=true;
    check(service.manage_network(owner,q,2).code==protocol::NetworkResultCode::success,"policy mutation");
    q.action=protocol::ManagementAction::inspect;const auto result=service.manage_network(owner,q,3);
    check(result.audit_events.size()==2 && result.audit_events[0].kind==protocol::AuditKind::policy_change && result.audit_events[0].policy_revision==1,"major operations recorded");
    check(protocol::decode_management_result(protocol::encode_management_result(result))==result,"audit management codec");
    protocol::AuditEvent event;event.network_id=id;event.actor=owner;event.kind=protocol::AuditKind::policy_change;event.timestamp_ms=100;
    for(unsigned i=0;i<300;++i){event.timestamp_ms=100+i;store.append_audit(event);}
    const auto bounded=store.audit_events(id,400,256);check(bounded.size()==256 && bounded.front().sequence>bounded.back().sequence,"per-network audit bound and ordering");
    event.timestamp_ms=31ULL*24*60*60*1000;store.append_audit(event);check(store.audit_events(id,static_cast<std::int64_t>(event.timestamp_ms),256).size()==1,"audit retention age");
    const auto encoded=protocol::encode_audit_event(event);check(encoded.size()==107 && protocol::decode_audit_event(encoded)==event,"bounded typed audit record");
    bool bad=false;try{static_cast<void>(protocol::decode_audit_event(std::span(encoded).first(106)));}catch(const std::exception&){bad=true;}check(bad,"truncated audit accepted");
  }{storage::RelayStore reopened(file);check(reopened.audit_events(id,31LL*24*60*60*1000).size()==1,"audit survives restart");}
  std::filesystem::remove(file);std::cout<<"audit types, atomic policy evidence, retention, bounds and restart passed\n";return 0;
}catch(const std::exception& e){std::filesystem::remove(file);std::cerr<<e.what()<<'\n';return 1;}}
