#include "lanlink/transport/relay_probe.hpp"
#include "lanlink/transport/tls.hpp"
#include <optional>
#include <stdexcept>

namespace lanlink::transport {
RelayProbe probe_relay(const auth::RelayEndpoint& endpoint,std::chrono::milliseconds timeout,const std::atomic_bool& stopping,bool accounting) {
    using Clock=std::chrono::steady_clock;
    if(timeout<std::chrono::milliseconds(50) || timeout>std::chrono::seconds(10) || stopping.load())throw std::invalid_argument("invalid relay probe deadline");
    const auto deadline=Clock::now()+timeout;
    auto channel=TlsChannel::connect(endpoint.host,endpoint.port,timeout,stopping);
    const auto nonce=auth::random_nonce();
    std::vector<std::byte> challenge(nonce.begin(),nonce.end());
    if(accounting)challenge.push_back(std::byte{2});
    if(!channel->enqueue({protocol::MessageType::relay_health_request,1,std::move(challenge)}))throw std::runtime_error("relay probe queue rejected challenge");
    const auto started=Clock::now();std::optional<RelayProbe> result;std::string failure;
    channel->run([&](const protocol::Frame& frame){
        try {
            if(result || !failure.empty() || frame.type!=protocol::MessageType::relay_health_result || frame.request_id!=1)throw std::runtime_error("unexpected relay health response");
            const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            auto health=auth::verify_relay_health(frame.payload,endpoint,nonce,now);
            if(accounting && !health.accounting_available)throw std::runtime_error("relay accounting signals unavailable");
            result=RelayProbe{std::move(health),std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-started)};
        }catch(const std::exception& error){failure=error.what();result.reset();}
    },[&]{return !stopping.load() && Clock::now()<deadline && !result && failure.empty();});
    if(stopping.load() || Clock::now()>=deadline || !result || !failure.empty())throw std::runtime_error(failure.empty()?"relay health probe stopped or timed out":failure);
    return *result;
}
}
