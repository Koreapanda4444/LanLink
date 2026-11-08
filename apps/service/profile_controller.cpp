#include "profile_controller.hpp"
#include "lanlink/transport/device_registration.hpp"
#include "lanlink/protocol/binary.hpp"
#include <openssl/crypto.h>
#include <algorithm>
namespace lanlink::service {
namespace {
std::filesystem::path directory(const core::RuntimeConfig& c){return c.device_identity_file.parent_path()/"profiles";}
bool mutates(protocol::LocalCommand c){using C=protocol::LocalCommand;return c==C::create || c==C::join || c==C::leave || c==C::invite || c==C::approve || c==C::kick || c==C::network_management || c==C::user_access || c==C::routing_options;}
}
ProfileController::ProfileController(core::RuntimeConfig c,const ServiceHooks& hooks):base_(std::move(c)),hooks_(hooks),profiles_(directory(base_)) {
    managed_=std::filesystem::exists(directory(base_)/"servers.state");
    const auto nonce=auth::random_nonce();std::uint64_t epoch=0;for(unsigned i=0;i<8;++i)epoch=(epoch<<8)|std::to_integer<unsigned>(nonce[i]);epoch_=(epoch & 0x7fffffffffffffffULL)+1;
    pipe_=std::make_unique<transport::LocalPipeServer>([this](const auto& q){return dispatch(q);});
}
ProfileController::~ProfileController(){if(pipe_)pipe_->stop();}
std::optional<core::RuntimeConfig> ProfileController::configuration()const {
    std::lock_guard lock(profiles_mutex_);const auto s=profiles_.state();
    if(s.selected_id.empty()) {if(managed_ || (base_.relay_host.empty() && base_.bootstrap_mode=="manual"))return std::nullopt;return base_;}
    const auto p=profiles_.get(s.selected_id);auto c=base_;
    c.bootstrap_mode="manual";c.relay_host=p.profile.host;c.relay_port=p.profile.port;c.server_public_key_pin=p.profile.public_key_pin;
    c.device_identity_file=profiles_.identity_path(s.selected_id);c.auth_token_file=profiles_.credential_path(s.selected_id);return c;
}
void ProfileController::update_base(core::RuntimeConfig c){std::lock_guard lock(profiles_mutex_);base_=std::move(c);++epoch_;}
void ProfileController::activate(Handler handler,std::uint64_t epoch){std::lock_guard lock(handler_mutex_);handler_=std::move(handler);handler_epoch_=epoch;idle_error_.clear();}
void ProfileController::idle(protocol::ClientConfigurationState state,std::string error){std::lock_guard lock(handler_mutex_);handler_={};handler_epoch_=0;idle_state_=state;idle_error_=std::move(error);}
protocol::ServerState ProfileController::server_request(const protocol::ServerRequest& q) {
    std::lock_guard lock(profiles_mutex_);const auto before=profiles_.state().connection_epoch;const auto was_managed=managed_;
    using A=protocol::ServerAction;
    switch(q.action) {
    case A::list:break;
    case A::save:result_id_=profiles_.save(q.profile,q.credential);managed_=true;break;
    case A::remove:profiles_.erase(q.profile.id);if(result_id_==q.profile.id)result_id_.clear();managed_=true;break;
    case A::connect:profiles_.select(q.profile.id);managed_=true;break;
    case A::disconnect:profiles_.select("");pending_join_.reset();managed_=true;notice_.clear();break;
    case A::enroll:enroll_locked(q.profile.id,q.registration_code,q.device_label);managed_=true;break;
    case A::accept_invitation: {
        if(q.expected_epoch!=epoch_)throw std::runtime_error("Server selection changed. Review the invitation again.");
        auto invitation=protocol::parse_server_invitation(q.invitation_uri);auto state=profiles_.state();std::string id;
        for(const auto& p:state.profiles)if(p.host==invitation.server.host && p.port==invitation.server.port && p.public_key_pin==invitation.server.public_key_pin){id=p.id;break;}
        if(!state.selected_id.empty() && state.selected_id!=id && !q.approve_server_switch)throw std::runtime_error("Confirm the server switch before connecting");
        if(id.empty() && !invitation.server.public_key_pin.empty() && !q.trust_invitation_pin)throw std::runtime_error("Confirm the server public key fingerprint before connecting");
        if(id.empty())id=profiles_.save(invitation.server);
        if(!profiles_.get(id).profile.has_credential && !invitation.registration_code.empty())enroll_locked(id,invitation.registration_code,q.device_label.empty()?"Invited device":q.device_label);
        profiles_.select(id);managed_=true;result_id_=id;
        if(std::any_of(invitation.network_id.begin(),invitation.network_id.end(),[](auto b){return b!=std::byte{0};}))pending_join_=PendingJoin{id,std::move(invitation)};
        notice_=profiles_.get(id).profile.has_credential?"Connecting to the invited server":"Device registration required";
        break;
    }
    }
    if(before!=profiles_.state().connection_epoch || (was_managed!=managed_ && !base_.relay_host.empty()))++epoch_;
    auto s=profiles_.state();s.connection_epoch=epoch_.load();s.result_profile_id=result_id_;s.notice=notice_;return s;
}
void ProfileController::enroll_locked(std::string_view id,std::string_view code,std::string_view label) {
    const auto p=profiles_.get(id);transport::QuicClientOptions options;options.relay_host=p.profile.host;options.port=p.profile.port;options.server_public_key_pin=p.profile.public_key_pin;options.handshake_timeout=std::chrono::seconds(3);options.authentication_timeout=std::chrono::seconds(3);
    const auto identity=auth::DeviceIdentity::load_or_create(profiles_.identity_path(id));std::atomic_bool stopping=false;
    auto secret=transport::register_device(options,identity,code,label,stopping);
    try {profiles_.save_device_credential(id,secret);}catch(...){OPENSSL_cleanse(secret.data(),secret.size());throw;}
    OPENSSL_cleanse(secret.data(),secret.size());notice_="Device registered";
}
void ProfileController::join_pending(transport::QuicRelayClient& client) {
    if(!client.authenticated())return;
    std::lock_guard lock(profiles_mutex_);
    if(!pending_join_)return;
    const auto selected=profiles_.state().selected_id;
    if(selected!=pending_join_->profile_id){pending_join_.reset();return;}
    const auto p=profiles_.get(selected).profile;const auto& target=pending_join_->invitation.server;
    if(p.host!=target.host || p.port!=target.port || p.public_key_pin!=target.public_key_pin){pending_join_.reset();return;}
    auto invitation=std::move(pending_join_->invitation);pending_join_.reset();
    protocol::NetworkResultCode code;
    if(invitation.network_invitation.empty())code=client.join_network(invitation.network_id).code;
    else {protocol::AccessRequest request;request.action=protocol::AccessAction::invite_redeem;request.network_id=invitation.network_id;request.secret=invitation.network_invitation;code=client.access_request(request).code;}
    notice_=code==protocol::NetworkResultCode::success?"Joined invited network":code==protocol::NetworkResultCode::pending_approval?"Waiting for network owner approval":"Network invitation rejected or expired";
}
protocol::LocalMessage ProfileController::dispatch(const protocol::LocalMessage& input) {
    namespace p=protocol;
    if(input.command==p::LocalCommand::server_profiles)return {input.command,true,false,p::encode_server_state(server_request(p::decode_server_request(input.payload)))};
    if(input.command==p::LocalCommand::stop) {if(!input.payload.empty())throw std::invalid_argument("stop does not accept a payload");hooks_.request_stop();return {input.command,true,false,{}};}
    p::LocalMessage q=input;bool scoped=false;std::uint64_t expected_epoch=0;
    if(input.command==p::LocalCommand::scoped_request) {
        p::BinaryReader r(input.payload);const auto expected=r.get<std::uint64_t>();expected_epoch=expected;const auto size=r.get<std::uint32_t>();q=p::decode_local_message(r.bytes(size));r.finish();
        if(q.response || q.error || q.command==p::LocalCommand::scoped_request || q.command==p::LocalCommand::server_profiles || q.command==p::LocalCommand::stop)throw std::invalid_argument("Invalid scoped service request");
        if(expected!=epoch_)throw std::runtime_error("Server selection changed. Refresh before retrying.");
        scoped=true;
    }
    bool managed;{std::lock_guard lock(profiles_mutex_);managed=managed_;}
    if(managed && mutates(q.command) && !scoped)throw std::runtime_error("Server-scoped authorization is required");
    std::lock_guard lock(handler_mutex_);
    if(scoped && expected_epoch!=epoch_)throw std::runtime_error("Server selection changed. Refresh before retrying.");
    if(handler_ && handler_epoch_==epoch_) {
        auto response=handler_(q);if(scoped){response.command=input.command;response.payload=p::encode_local_message({q.command,true,response.error,std::move(response.payload)});}return response;
    }
    std::vector<std::byte> payload;
    switch(q.command) {
    case p::LocalCommand::status:if(!q.payload.empty())throw std::invalid_argument("status does not accept a payload");payload.resize(8);payload[3]=static_cast<std::byte>(idle_state_);break;
    case p::LocalCommand::diagnostics:{if(!q.payload.empty())throw std::invalid_argument("diagnostics does not accept a payload");p::LocalDiagnostics d;d.configuration=idle_state_;d.last_error=idle_error_;payload=p::encode_local_diagnostics(d);break;}
    case p::LocalCommand::list:static_cast<void>(p::decode_network_list_request(q.payload));payload=p::encode_network_list_result({});break;
    case p::LocalCommand::activity:if(!q.payload.empty())throw std::invalid_argument("activity does not accept a payload");payload=p::encode_local_activity({});break;
    case p::LocalCommand::path_diagnostics:if(!q.payload.empty())throw std::invalid_argument("path diagnostics does not accept a payload");payload=p::encode_path_diagnostics({});break;
    case p::LocalCommand::game_diagnostics:if(!q.payload.empty())throw std::invalid_argument("game diagnostics does not accept a payload");payload=p::encode_game_diagnostics({});break;
    default:throw std::runtime_error(idle_error_.empty()?"Connect to a server first":idle_error_);
    }
    if(scoped)payload=p::encode_local_message({q.command,true,false,std::move(payload)});
    return {input.command,true,false,std::move(payload)};
}
}
