#include "lanlink/protocol/network_workflow.hpp"
#include "lanlink/protocol/join_target.hpp"
#include <stdexcept>
namespace lanlink::protocol {
LocalMessage create_network_command(std::string_view name,std::string_view password,bool require_approval) {
    static_cast<void>(encode_network_create_request({std::string(name)}));
    if(password.empty())return {LocalCommand::create,false,false,encode_network_create_request({std::string(name)})};
    if(password.size()<8 || password.size()>128)throw std::invalid_argument("Password must contain 8 to 128 bytes.");
    AccessRequest r;r.action=AccessAction::network_create_protected;r.text=name;r.secret=password;r.require_approval=require_approval;
    return {LocalCommand::user_access,false,false,encode_access_request(r)};
}
LocalMessage join_network_command(std::string_view text,std::string_view password) {
    const auto target=parse_join_target(text);
    if(target.invitation.empty() && password.empty())return {LocalCommand::join,false,false,encode_network_selection_request({target.network_id})};
    AccessRequest r;r.network_id=target.network_id;
    if(!target.invitation.empty()){r.action=AccessAction::invite_redeem;r.secret=target.invitation;}
    else {
        if(password.size()<8 || password.size()>128)throw std::invalid_argument("Password must contain 8 to 128 bytes.");
        r.action=AccessAction::password_join;r.secret=password;
    }
    return {LocalCommand::user_access,false,false,encode_access_request(r)};
}
}
