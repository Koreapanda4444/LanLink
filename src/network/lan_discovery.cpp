#include "lanlink/network/lan_discovery.hpp"

#include <openssl/sha.h>
#include <algorithm>

namespace lanlink::network {
namespace {
std::uint32_t integer(std::span<const std::byte> bytes, std::size_t offset, std::size_t length) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < length; ++index)
        value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + index]);
    return value;
}
}

bool LanDiscoveryGuard::permit(std::span<const std::byte> packet, const Clock::time_point now) {
    if (packet.size() < 20) { ++counters_.invalid; return false; }
    const auto header = (std::to_integer<unsigned char>(packet[0]) & 15U) * 4U;
    if ((std::to_integer<unsigned char>(packet[0]) >> 4U) != 4 || header < 20 ||
        header > packet.size() || integer(packet, 2, 2) != packet.size() || packet[8] == std::byte{0}) {
        ++counters_.invalid; return false;
    }
    const auto destination = integer(packet, 16, 4);
    const bool discovery = destination == 0xe00000fbU || destination == 0xe00000fcU || destination == 0xeffffffaU;
    if (discovery) {
        const auto port = destination == 0xe00000fbU ? 5353U : destination == 0xe00000fcU ? 5355U : 1900U;
        if (packet[9] != std::byte{17} || (integer(packet, 6, 2) & 0x3fffU) != 0 ||
            packet.size() - header < 8 || integer(packet, header + 2, 2) != port ||
            integer(packet, header + 4, 2) != packet.size() - header ||
            (destination == 0xe00000fbU && packet[8] != std::byte{255})) {
            ++counters_.invalid; return false;
        }
    }
    if (!initialized_) { updated_ = now; initialized_ = true; }
    if (now > updated_) {
        const auto seconds = std::chrono::duration<double>(now - updated_).count();
        packets_ = std::min(32.0, packets_ + seconds * 16.0);
        bytes_ = std::min(65536.0, bytes_ + seconds * 32768.0);
        updated_ = now;
    }
    std::erase_if(recent_, [now](const auto& entry) { return now - entry.second >= std::chrono::milliseconds{100}; });
    std::array<unsigned char, 32> digest{};
    if (discovery) {
        if (!SHA256(reinterpret_cast<const unsigned char*>(packet.data()), packet.size(), digest.data())) {
            ++counters_.invalid; return false;
        }
        if (recent_.contains(digest)) { ++counters_.duplicate; return false; }
    }
    if (packets_ < 1 || bytes_ < static_cast<double>(packet.size())) { ++counters_.rate_limited; return false; }
    --packets_; bytes_ -= static_cast<double>(packet.size());
    if (discovery) {
        if (recent_.size() == 128) recent_.erase(std::min_element(recent_.begin(), recent_.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; }));
        recent_.emplace(digest, now);
    }
    ++counters_.forwarded;
    return true;
}
}
