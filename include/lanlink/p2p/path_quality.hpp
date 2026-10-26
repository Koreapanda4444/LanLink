#pragma once
#include "lanlink/protocol/path_diagnostics.hpp"
#include <chrono>
namespace lanlink::p2p {
struct QualitySample {
    bool authenticated=false;
    std::optional<std::uint32_t> rtt_us;
    std::uint64_t transport_sent_packets=0,lost_packets=0;
};
struct PathDecision {bool direct=false;protocol::PathReason reason=protocol::PathReason::direct_unreachable;};
class PathSelector {
public:
    using Clock=std::chrono::steady_clock;
    [[nodiscard]] PathDecision sample(const QualitySample& direct,std::optional<std::uint32_t> relay_rtt,
        Clock::time_point now=Clock::now());
    [[nodiscard]] std::optional<std::uint32_t> smoothed_rtt_us()const noexcept{return rtt_;}
    [[nodiscard]] std::optional<std::uint32_t> loss_basis_points()const noexcept{return loss_;}
private:
    bool direct_=false,seen_=false;unsigned healthy_=0,bad_=0;
    std::uint64_t sent_=0,lost_=0;std::optional<std::uint32_t> rtt_,loss_;
    Clock::time_point changed_{};
};
}
