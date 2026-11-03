#pragma once
#include "lanlink/auth/identity.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

namespace lanlink::relay {
struct ForwardingLimits {
    std::uint64_t device_bytes_per_second=8U*1024U*1024U;
    std::uint64_t network_bytes_per_second=32U*1024U*1024U;
    std::uint32_t device_deliveries_per_second=8192,network_deliveries_per_second=32768;
    std::uint32_t control_per_second=32;
    std::uint64_t authority_bytes_per_second=64U*1024U*1024U;
    std::uint32_t authority_deliveries_per_second=65536;
};
struct ForwardingMetrics {
    std::uint64_t admitted_deliveries=0,admitted_bytes=0,quota_rejections=0,control_rejections=0;
};
class ForwardingQuota {
public:
    using Clock=std::chrono::steady_clock;
    void configure(ForwardingLimits limits);
    [[nodiscard]] bool admit(const auth::DeviceId&,const protocol::NetworkId&,std::uint64_t bytes_per_delivery,std::uint32_t recipients,Clock::time_point now);
    [[nodiscard]] bool control(const auth::DeviceId&,Clock::time_point now);
    [[nodiscard]] ForwardingMetrics metrics()const;
private:
    struct Bucket {double bytes=0,deliveries=0,controls=0;Clock::time_point updated{};bool initialized=false;};
    static void refill(Bucket&,std::uint64_t bytes,std::uint32_t deliveries,std::uint32_t controls,Clock::time_point now);
    void prune(Clock::time_point now);
    mutable std::mutex mutex_;ForwardingLimits limits_;ForwardingMetrics metrics_;
    std::map<auth::DeviceId,Bucket> devices_;
    std::map<protocol::NetworkId,Bucket> networks_;
    Clock::time_point next_prune_{};
    Bucket aggregate_;
};
}
