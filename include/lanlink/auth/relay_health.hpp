#pragma once
#include "lanlink/auth/directory.hpp"

namespace lanlink::auth {
struct RelayHealth {
    Nonce nonce{};
    std::string relay_id;
    std::string authority;
    std::uint64_t issued_ms=0;
    std::uint64_t uptime_ms=0;
    std::uint32_t capacity=0;
    std::uint32_t active_connections=0;
    std::uint32_t upstream_rtt_us=0;
    std::uint64_t forwarded_packets=0;
    std::uint64_t forwarded_bytes=0;
    std::uint64_t dropped_packets=0;
    std::uint64_t admitted_deliveries=0,admitted_bytes=0,quota_rejections=0,control_rejections=0;
    bool accounting_available=false;
    bool ready=false;
    bool operator==(const RelayHealth&) const=default;
};
[[nodiscard]] std::vector<std::byte> sign_relay_health(const RelayHealth&,const DeviceIdentity&);
[[nodiscard]] RelayHealth verify_relay_health(std::span<const std::byte>,const RelayEndpoint&,const Nonce&,std::uint64_t now_ms);
}
