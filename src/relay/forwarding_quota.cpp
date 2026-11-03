#include "lanlink/relay/forwarding_quota.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lanlink::relay {
void ForwardingQuota::configure(ForwardingLimits limits) {
    if(!limits.device_bytes_per_second || limits.device_bytes_per_second>1024ULL*1024*1024 ||
       !limits.network_bytes_per_second || limits.network_bytes_per_second>1024ULL*1024*1024 ||
       !limits.device_deliveries_per_second || limits.device_deliveries_per_second>1'000'000 ||
       !limits.network_deliveries_per_second || limits.network_deliveries_per_second>1'000'000 ||
       !limits.authority_bytes_per_second || limits.authority_bytes_per_second>1024ULL*1024*1024 ||
       !limits.authority_deliveries_per_second || limits.authority_deliveries_per_second>1'000'000 ||
       !limits.control_per_second || limits.control_per_second>10'000)throw std::invalid_argument("invalid forwarding quota");
    std::lock_guard lock(mutex_);limits_=limits;devices_.clear();networks_.clear();aggregate_={};
}
void ForwardingQuota::refill(Bucket& bucket,std::uint64_t bytes,std::uint32_t deliveries,std::uint32_t controls,Clock::time_point now) {
    constexpr double burst=2;
    if(!bucket.initialized) {
        bucket.bytes=static_cast<double>(bytes)*burst;bucket.deliveries=static_cast<double>(deliveries)*burst;
        bucket.controls=static_cast<double>(controls)*burst;bucket.initialized=true;bucket.updated=now;return;
    }
    if(now<=bucket.updated)return;
    const double seconds=std::chrono::duration<double>(now-bucket.updated).count();
    bucket.bytes=(std::min)(static_cast<double>(bytes)*burst,bucket.bytes+seconds*static_cast<double>(bytes));
    bucket.deliveries=(std::min)(static_cast<double>(deliveries)*burst,bucket.deliveries+seconds*deliveries);
    bucket.controls=(std::min)(static_cast<double>(controls)*burst,bucket.controls+seconds*controls);bucket.updated=now;
}
void ForwardingQuota::prune(Clock::time_point now) {
    if(now<next_prune_)return;
    const auto expired=[&](const auto& item){return now>item.second.updated && now-item.second.updated>std::chrono::minutes(5);};
    std::erase_if(devices_,expired);std::erase_if(networks_,expired);next_prune_=now+std::chrono::seconds(30);
}
bool ForwardingQuota::control(const auth::DeviceId& actor,Clock::time_point now) {
    std::lock_guard lock(mutex_);prune(now);
    if(!devices_.contains(actor) && devices_.size()>=4096){++metrics_.control_rejections;return false;}
    auto& device=devices_[actor];refill(device,limits_.device_bytes_per_second,limits_.device_deliveries_per_second,limits_.control_per_second,now);
    if(device.controls<1){++metrics_.control_rejections;return false;}--device.controls;return true;
}
bool ForwardingQuota::admit(const auth::DeviceId& actor,const protocol::NetworkId& network,std::uint64_t size,std::uint32_t recipients,Clock::time_point now) {
    std::lock_guard lock(mutex_);prune(now);
    if(!size || !recipients || recipients>4096 || size>std::numeric_limits<std::uint64_t>::max()/recipients ||
       (!devices_.contains(actor) && devices_.size()>=4096) || (!networks_.contains(network) && networks_.size()>=4096)) {
        ++metrics_.quota_rejections;return false;
    }
    auto& device=devices_[actor];auto& shared=networks_[network];
    refill(device,limits_.device_bytes_per_second,limits_.device_deliveries_per_second,limits_.control_per_second,now);
    refill(shared,limits_.network_bytes_per_second,limits_.network_deliveries_per_second,limits_.control_per_second,now);
    refill(aggregate_,limits_.authority_bytes_per_second,limits_.authority_deliveries_per_second,limits_.control_per_second,now);
    const auto bytes=size*recipients;
    if(device.bytes<static_cast<double>(bytes) || shared.bytes<static_cast<double>(bytes) ||
       device.deliveries<recipients || shared.deliveries<recipients || aggregate_.bytes<static_cast<double>(bytes) || aggregate_.deliveries<recipients){++metrics_.quota_rejections;return false;}
    device.bytes-=static_cast<double>(bytes);shared.bytes-=static_cast<double>(bytes);
    device.deliveries-=recipients;shared.deliveries-=recipients;
    aggregate_.bytes-=static_cast<double>(bytes);aggregate_.deliveries-=recipients;
    metrics_.admitted_deliveries+=recipients;metrics_.admitted_bytes+=bytes;return true;
}
ForwardingMetrics ForwardingQuota::metrics()const{std::lock_guard lock(mutex_);return metrics_;}
}
