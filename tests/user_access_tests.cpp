#include "lanlink/relay/network_control_channel.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace lanlink;
void require(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
int main() {
    const auto dir=std::filesystem::temp_directory_path()/auth::random_access_code();
    std::filesystem::create_directories(dir);
    try {
        auto owner=auth::random_nonce(), device=auth::random_nonce(), outsider=auth::random_nonce();
        {
            storage::RelayStore store(dir/"relay.db"); relay::NetworkService service(store);
            std::int64_t now=1000; relay::NetworkControlChannel channel(service,[&]{return now;});
            for (const auto& actor:{owner,device,outsider}) {
                channel.note_authenticated(actor);
                static_cast<void>(channel.handle_authenticated(actor,{protocol::MessageType::network_list_request,1,protocol::encode_capability_offer({})}));
            }
            auto send=[&](const auth::DeviceId& actor, protocol::AccessRequest request) {
                const auto dispatch=channel.handle_authenticated(actor,{protocol::MessageType::user_access_request,2,protocol::encode_access_request(request)});
                require(dispatch.response.type==protocol::MessageType::user_access_result,"access response");
                return protocol::decode_access_result(dispatch.response.payload);
            };
            protocol::AccessRequest request; request.action=protocol::AccessAction::account_create; request.text="Panda";
            require(send(owner,request).devices.size()==1,"optional account creation");
            request={}; request.action=protocol::AccessAction::enrollment_issue;
            const auto code=send(owner,request).secret; require(code.size()==40,"random enrollment code");
            require(send(outsider,request).code==protocol::NetworkResultCode::permission_denied,"outsider cannot issue enrollment");
            request.action=protocol::AccessAction::device_enroll; request.secret=code; request.text="Desktop";
            require(send(device,request).devices.size()==2,"device enrollment");
            require(send(outsider,request).code==protocol::NetworkResultCode::permission_denied,"code is single use");
            request={}; request.action=protocol::AccessAction::enrollment_issue;
            auto expired=send(owner,request).secret; now+=300000;
            request.action=protocol::AccessAction::device_enroll; request.secret=expired; request.text="expired";
            require(send(outsider,request).code==protocol::NetworkResultCode::permission_denied,"enrollment expires at boundary");
            const auto network=service.create_network(owner,{"Room"},now).result.network_id;
            static_cast<void>(service.invite_member(owner,{network,device},now));
            require(service.join_network(device,{network},now).result.code==protocol::NetworkResultCode::success,"enrolled device still uses legacy networking");
            request={}; request.action=protocol::AccessAction::device_revoke; request.device_id=device;
            const auto revoked=channel.handle_authenticated(owner,{protocol::MessageType::user_access_request,3,protocol::encode_access_request(request)});
            require(protocol::decode_access_result(revoked.response.payload).code==protocol::NetworkResultCode::success,"device revoked");
            require(!revoked.events.empty() && !store.find_virtual_ipv4_lease(network,device),"revocation removes membership and sends peer/key invalidation");
            require(channel.handle_authenticated(device,{protocol::MessageType::network_list_request,4,{}}).response.type==protocol::MessageType::error,"revoked device denied control");
            require(service.create_network(outsider,{"No account required"},now).result.code==protocol::NetworkResultCode::success,"token-only client unchanged");
        }
        { storage::RelayStore reopened(dir/"relay.db"); require(reopened.device_revoked(device),"revocation survives restart"); }
        std::filesystem::remove_all(dir); std::cout << "account enrollment and revocation passed\n"; return 0;
    } catch (const std::exception& e) { std::filesystem::remove_all(dir); std::cerr<<e.what()<<'\n'; return 1; }
}
