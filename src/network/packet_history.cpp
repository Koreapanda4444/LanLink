#include "lanlink/network/packet_history.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace lanlink::network {
PacketHistory::Digest PacketHistory::fingerprint(std::span<const std::byte> packet) {
    std::vector<std::byte> normalized(packet.begin(), packet.end());
    if (normalized.size() >= 20) normalized[8] = normalized[10] = normalized[11] = std::byte{0};
    Digest result{};
    if (!SHA256(reinterpret_cast<const unsigned char*>(normalized.data()), normalized.size(), result.data()))
        throw std::runtime_error("packet fingerprint unavailable");
    return result;
}
void PacketHistory::expire(const Clock::time_point now) {
    std::erase_if(recent_, [now](const auto& entry) { return now - entry.second >= std::chrono::seconds{2}; });
}
bool PacketHistory::contains(std::span<const std::byte> packet, const Clock::time_point now) {
    expire(now); return recent_.contains(fingerprint(packet));
}
void PacketHistory::remember(std::span<const std::byte> packet, const Clock::time_point now) {
    expire(now);
    if (recent_.size() >= 256) recent_.erase(std::min_element(recent_.begin(), recent_.end(),
        [](const auto& left, const auto& right) { return left.second < right.second; }));
    recent_.insert_or_assign(fingerprint(packet), now);
}
}
