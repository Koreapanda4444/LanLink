#include "lanlink/p2p/path_quality.hpp"
#include <algorithm>
namespace lanlink::p2p {
PathDecision PathSelector::sample(const QualitySample& d,std::optional<std::uint32_t> relay,Clock::time_point now) {
    if(!d.authenticated){if(direct_)changed_=now;direct_=false;healthy_=bad_=0;rtt_.reset();loss_.reset();seen_=false;return {false,protocol::PathReason::direct_unreachable};}
    if(d.rtt_us)rtt_=rtt_?static_cast<std::uint32_t>((3ULL*(*rtt_)+*d.rtt_us)/4):*d.rtt_us;
    if(seen_ && d.transport_sent_packets>=sent_ && d.lost_packets>=lost_ && d.transport_sent_packets>sent_) {
        const auto count=d.transport_sent_packets-sent_,lost=std::min(d.lost_packets-lost_,count);
        const auto current=static_cast<std::uint32_t>(lost*10000/count);
        loss_=loss_?static_cast<std::uint32_t>((3ULL*(*loss_)+current)/4):current;
    }
    sent_=d.transport_sent_packets;lost_=d.lost_packets;seen_=true;
    // Relay RTT is measured to the server, so it is only a conservative guard, not a peer RTT estimate.
    const std::uint64_t ceiling=std::max<std::uint64_t>(250000,relay?4ULL*(*relay):250000);
    const bool good=rtt_ && *rtt_<=ceiling && (!loss_ || *loss_<=1000);
    if(good){healthy_=std::min(healthy_+1,3U);bad_=0;}else{bad_=std::min(bad_+1,3U);healthy_=0;}
    const bool hold=changed_!=Clock::time_point{} && now-changed_<std::chrono::seconds{2};
    if(!hold && ((direct_ && bad_>=3) || (!direct_ && healthy_>=3))){direct_=!direct_;changed_=now;}
    return {direct_,direct_?protocol::PathReason::direct_ready:good?protocol::PathReason::revalidating:protocol::PathReason::degraded};
}
}
