#pragma once

#include "lanlink/protocol/network_messages.hpp"

namespace lanlink::protocol {

struct GameNetworkStatus {
    NetworkId network_id{};
    std::uint32_t address = 0, subnet = 0;
    std::uint8_t prefix_length = 24;
    std::uint32_t online_peers = 0;
    std::uint64_t discovery_sent = 0, discovery_received = 0;
    std::uint64_t rate_limited = 0, duplicates = 0, invalid = 0, reflections = 0;
    bool operator==(const GameNetworkStatus&) const = default;
};
struct GameDiagnostics {
    std::uint16_t mtu = 1400;
    bool adapter_failed = false, truncated = false;
    std::uint64_t rejected_packets = 0;
    std::vector<GameNetworkStatus> networks;
    bool operator==(const GameDiagnostics&) const = default;
};
inline constexpr std::size_t max_game_diagnostic_networks = 256;
[[nodiscard]] std::vector<std::byte> encode_game_diagnostics(const GameDiagnostics& value);
[[nodiscard]] GameDiagnostics decode_game_diagnostics(std::span<const std::byte> bytes);

}
