#pragma once
#include "lanlink/auth/directory.hpp"
#include "lanlink/transport/tls.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace lanlink::transport {
// A regional front carries the original client proof to the one persistent authority.
// The front cannot enroll a device or issue network keys on behalf of that authority.
class RelayBridge {
public:
    using Delivery=std::function<bool(const protocol::Frame&)>;
    RelayBridge(auth::RelayEndpoint upstream,std::chrono::milliseconds connect_timeout,
        std::chrono::milliseconds auth_timeout,SendQueueLimits limits,Delivery deliver,std::function<void()> closed);
    ~RelayBridge();
    RelayBridge(const RelayBridge&)=delete;RelayBridge& operator=(const RelayBridge&)=delete;
    [[nodiscard]] bool send(const protocol::Frame&);
    [[nodiscard]] bool authenticated()const noexcept{return authenticated_.load();}
    [[nodiscard]] bool finished()const noexcept{return finished_.load();}
    void stop()noexcept;
    void join()noexcept;
private:
    void run()noexcept;
    auth::RelayEndpoint upstream_;
    std::chrono::milliseconds connect_timeout_,auth_timeout_;
    Delivery deliver_;std::function<void()> closed_;
    SendBudget budget_;
    struct Queued {protocol::Frame frame;SendBudget::Ticket ticket;};
    std::mutex mutex_;std::deque<Queued> queued_;
    std::shared_ptr<TlsChannel> channel_;
    std::atomic_bool stopping_=false,authenticated_=false,finished_=false;
    std::jthread worker_;
};
}
