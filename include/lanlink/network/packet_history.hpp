#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <span>

namespace lanlink::network {
class PacketHistory {
public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] bool contains(std::span<const std::byte> packet, Clock::time_point now);
    void remember(std::span<const std::byte> packet, Clock::time_point now);
private:
    using Digest = std::array<unsigned char, 32>;
    [[nodiscard]] static Digest fingerprint(std::span<const std::byte> packet);
    void expire(Clock::time_point now);
    std::map<Digest, Clock::time_point> recent_;
};
}
