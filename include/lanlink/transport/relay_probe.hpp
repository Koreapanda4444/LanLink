#pragma once
#include "lanlink/auth/relay_health.hpp"
#include <atomic>
#include <chrono>

namespace lanlink::transport {
struct RelayProbe {
    auth::RelayHealth health;
    std::chrono::microseconds rtt{};
};
[[nodiscard]] RelayProbe probe_relay(const auth::RelayEndpoint&,std::chrono::milliseconds timeout,const std::atomic_bool& stopping);
}
