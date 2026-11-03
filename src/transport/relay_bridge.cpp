#include "lanlink/transport/relay_bridge.hpp"
#include "lanlink/auth/handshake.hpp"
#include <stdexcept>

namespace lanlink::transport {
RelayBridge::RelayBridge(auth::RelayEndpoint upstream,std::chrono::milliseconds connect_timeout,
    std::chrono::milliseconds auth_timeout,SendQueueLimits limits,Delivery deliver,std::function<void()> closed)
    :upstream_(std::move(upstream)),connect_timeout_(connect_timeout),auth_timeout_(auth_timeout),
     deliver_(std::move(deliver)),closed_(std::move(closed)),budget_(limits),worker_([this]{run();}){}
RelayBridge::~RelayBridge(){stop();join();}
void RelayBridge::join()noexcept{if(worker_.joinable())worker_.join();}
void RelayBridge::stop()noexcept{
    stopping_=true;std::shared_ptr<TlsChannel> channel;
    {std::lock_guard lock(mutex_);queued_.clear();channel=channel_;}
    if(channel)channel->stop();
}
bool RelayBridge::send(const protocol::Frame& frame) {
    if(stopping_.load() || finished_.load())return false;
    const bool data=frame.type==protocol::MessageType::network_packet_send;
    auto ticket=budget_.reserve(frame.payload.size()+12,!data);if(!ticket)return false;
    std::lock_guard lock(mutex_);
    if(stopping_.load() || finished_.load())return false;
    queued_.push_back({frame,std::move(*ticket)});return true;
}
void RelayBridge::run()noexcept {
    try {
        auto channel=TlsChannel::connect(upstream_.host,upstream_.port,connect_timeout_,stopping_);
        {std::lock_guard lock(mutex_);channel_=channel;}
        const auto deadline=std::chrono::steady_clock::now()+auth_timeout_;
        channel->run([this](const protocol::Frame& frame){
            if(frame.type==protocol::MessageType::auth_result) {
                if(authenticated_.load())throw std::runtime_error("duplicate upstream authentication result");
                const auto result=auth::decode_auth_result(frame.payload);authenticated_=result.accepted;
            }
            if(!deliver_(frame))throw std::runtime_error("regional downstream backpressure");
        },[&]{
            if(stopping_.load() || (!authenticated_.load() && std::chrono::steady_clock::now()>=deadline))return false;
            std::deque<Queued> queued;{std::lock_guard lock(mutex_);queued.swap(queued_);}
            for(const auto& item:queued)if(!channel->enqueue(item.frame))return false;
            return true;
        });
    }catch(const std::exception&){}
    authenticated_=false;
    {std::lock_guard lock(mutex_);channel_.reset();queued_.clear();}
    try{closed_();}catch(...){}
    finished_=true;
}
}
