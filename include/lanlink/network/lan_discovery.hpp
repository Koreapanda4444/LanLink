#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>

namespace lanlink::network {

struct DiscoveryCounters {
    std::uint64_t forwarded = 0, rate_limited = 0, duplicate = 0, invalid = 0;
};

class LanDiscoveryGuard {
public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] bool permit(std::span<const std::byte> packet, Clock::time_point now);
    [[nodiscard]] DiscoveryCounters counters() const noexcept { return counters_; }
private:
    bool initialized_ = false;
    double packets_ = 32, bytes_ = 65536;
    Clock::time_point updated_{};
    std::map<std::array<unsigned char, 32>, Clock::time_point> recent_;
    DiscoveryCounters counters_;
};

}
