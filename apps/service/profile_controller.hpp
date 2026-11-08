#pragma once
#include "lanlink/client/profile_store.hpp"
#include "lanlink/core/config.hpp"
#include "lanlink/platform/service_runtime.hpp"
#include "lanlink/protocol/local_control.hpp"
#include "lanlink/transport/local_pipe.hpp"
#include "lanlink/transport/quic.hpp"
#include <atomic>
#include <mutex>
namespace lanlink::service {
class ProfileController {
public:
    using Handler=std::function<protocol::LocalMessage(const protocol::LocalMessage&)>;
    ProfileController(core::RuntimeConfig config,const ServiceHooks& hooks);
    ~ProfileController();
    std::uint64_t epoch()const{return epoch_.load();}
    std::optional<core::RuntimeConfig> configuration()const;
    void join_pending(transport::QuicRelayClient& client);
    void update_base(core::RuntimeConfig config);
    void activate(Handler handler,std::uint64_t epoch);
    void idle(protocol::ClientConfigurationState state,std::string error = {});
private:
    protocol::LocalMessage dispatch(const protocol::LocalMessage& request);
    protocol::ServerState server_request(const protocol::ServerRequest& request);
    mutable std::mutex profiles_mutex_,handler_mutex_;
    core::RuntimeConfig base_;
    const ServiceHooks& hooks_;
    client::ProfileStore profiles_;
    bool managed_=false;
    struct PendingJoin { std::string profile_id; protocol::ServerInvitation invitation; unsigned attempts=0; std::chrono::steady_clock::time_point retry_at{}; };
    std::optional<PendingJoin> pending_join_;
    std::string notice_,result_id_;
    void enroll_locked(std::string_view id,std::string_view code,std::string_view label);
    std::atomic<std::uint64_t> epoch_{1};
    Handler handler_;
    std::uint64_t handler_epoch_=0;
    protocol::ClientConfigurationState idle_state_=protocol::ClientConfigurationState::unconfigured;
    std::string idle_error_;
    std::unique_ptr<transport::LocalPipeServer> pipe_;
};
}
