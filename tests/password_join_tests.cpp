#include "lanlink/relay/network_service.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace lanlink;
void require(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
int main(){
    const auto path=std::filesystem::temp_directory_path()/auth::random_access_code();
    try {
        storage::RelayStore store(path); relay::NetworkService::Clock::time_point now{};
        relay::NetworkService service(store,{},[&]{return now;});
        const auto owner=auth::random_nonce(),member=auth::random_nonce(),pending=auth::random_nonce();
        const auto network=service.create_network(owner,{"Protected"},1000).result.network_id;
        protocol::AccessRequest atomic;atomic.action=protocol::AccessAction::network_create_protected;
        atomic.text="Atomic room";atomic.secret="atomic password";
        const auto created=service.access_request(owner,atomic,1000);
        require(created.code==protocol::NetworkResultCode::success && store.network_password(created.network_id).has_value(),"password committed with network creation");
        require(service.join_network(member,{created.network_id},1000).result.code==protocol::NetworkResultCode::invite_required,"new room is never briefly unprotected");
        protocol::AccessRequest r;r.network_id=network;r.action=protocol::AccessAction::password_set;r.secret="correct horse battery";
        require(service.access_request(owner,r,1000).code==protocol::NetworkResultCode::success,"password saved");
        require(service.join_network(member,{network},1000).result.code==protocol::NetworkResultCode::invite_required,"legacy join cannot bypass password");
        r.action=protocol::AccessAction::password_join;r.secret="incorrect password";
        require(service.access_request(member,r,1000).code==protocol::NetworkResultCode::permission_denied,"wrong password rejected");
        r.secret="correct horse battery";
        require(service.access_request(member,r,1000).code==protocol::NetworkResultCode::success,"password joins without account");
        r.action=protocol::AccessAction::password_set;r.require_approval=true;
        require(service.access_request(owner,r,1000).code==protocol::NetworkResultCode::success,"owner enables approval");
        r.action=protocol::AccessAction::password_join;
        require(service.access_request(pending,r,1000).code==protocol::NetworkResultCode::pending_approval,"password preserves administrator approval");
        require(!store.find_virtual_ipv4_lease(network,pending),"pending device has no data access");
        require(service.approve_member(owner,{network,pending},1000).result.code==protocol::NetworkResultCode::success,"owner can approve password request");
        const auto attacker=auth::random_nonce();r.secret="wrong password";
        for(unsigned i=0;i<5;++i) require(service.access_request(attacker,r,1000).code==protocol::NetworkResultCode::permission_denied,"bounded password failure");
        require(service.access_request(attacker,r,1000).code==protocol::NetworkResultCode::limit_reached,"brute force rate limited");
        now+=std::chrono::minutes(1);
        require(service.access_request(attacker,r,61000).code==protocol::NetworkResultCode::permission_denied,"rate limit expires");
        require(store.network_password(network).has_value(),"salted verifier persisted");
        std::filesystem::remove(path);std::cout<<"password policy passed\n";return 0;
    } catch(const std::exception& e){std::filesystem::remove(path);std::cerr<<e.what()<<'\n';return 1;}
}
