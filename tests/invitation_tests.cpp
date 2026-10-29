#include "lanlink/relay/network_service.hpp"
#include "lanlink/protocol/join_target.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace lanlink;
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){
    const auto path=std::filesystem::temp_directory_path()/auth::random_access_code();
    try {
        storage::RelayStore store(path);relay::NetworkService::Clock::time_point time{};
        relay::NetworkService service(store,{},[&]{return time;});
        const auto owner=auth::random_nonce(),first=auth::random_nonce(),second=auth::random_nonce();
        const auto id=service.create_network(owner,{"Invitations"},1000).result.network_id;
        protocol::AccessRequest r;r.action=protocol::AccessAction::invite_issue;r.network_id=id;r.lifetime_seconds=60;
        require(service.access_request(first,r,1000).code==protocol::NetworkResultCode::permission_denied,"only owner issues links");
        const auto issued=service.access_request(owner,r,1000);auto target=protocol::parse_join_target(issued.secret);
        require(target.network_id==id && protocol::parse_join_target(issued.text).invitation==target.invitation,"link and code agree");
        r.action=protocol::AccessAction::invite_redeem;r.secret=target.invitation;
        require(service.access_request(first,r,1000).code==protocol::NetworkResultCode::success,"one-time invitation grants membership");
        require(service.access_request(second,r,1000).code==protocol::NetworkResultCode::permission_denied,"one-time invitation cannot be reused");
        time+=std::chrono::minutes(1);r.action=protocol::AccessAction::invite_issue;r.max_uses=2;
        const auto multi=service.access_request(owner,r,2000);target=protocol::parse_join_target(multi.secret);
        r.action=protocol::AccessAction::invite_redeem;r.secret=target.invitation;
        require(service.access_request(second,r,2000).code==protocol::NetworkResultCode::success,"bounded multi-use invitation");
        const auto third=auth::random_nonce();
        require(service.access_request(third,r,62000).code==protocol::NetworkResultCode::permission_denied,"expires at boundary");
        time+=std::chrono::minutes(1);r.action=protocol::AccessAction::invite_issue;
        const auto canceled=service.access_request(owner,r,63000);target=protocol::parse_join_target(canceled.secret);
        r.action=protocol::AccessAction::invite_revoke;
        require(service.access_request(owner,r,63000).code==protocol::NetworkResultCode::success,"owner revokes unredeemed links");
        r.action=protocol::AccessAction::invite_redeem;r.secret=target.invitation;
        require(service.access_request(third,r,63000).code==protocol::NetworkResultCode::permission_denied,"revoked link rejected");
        require(store.list_members(id).size()==3,"invite never grants ownership");
        for(auto bad:{"https://bad.example/", "lanlink://join/abc", "LL1.00000000000000000000000000000000.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
            bool rejected=false;try{static_cast<void>(protocol::parse_join_target(bad));}catch(const std::exception&){rejected=true;}require(rejected,"malformed link rejected");
        }
        std::filesystem::remove(path);std::cout<<"invitation policy passed\n";return 0;
    }catch(const std::exception& e){std::filesystem::remove(path);std::cerr<<e.what()<<'\n';return 1;}
}
