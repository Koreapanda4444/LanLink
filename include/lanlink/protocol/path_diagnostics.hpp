#pragma once
#include "lanlink/core/error.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include <optional>

namespace lanlink::protocol {
enum class PathKind : std::uint8_t { none, relay_quic, relay_tls, direct_quic };
enum class PathLifecycle : std::uint8_t { stopped, connecting, authenticated, failed };
enum class PathReason : std::uint8_t { awaiting_relay, relayed, direct_ready, direct_unreachable,
                                     udp_blocked, degraded, revalidating, revoked, stopped };
struct PathStatus {
    PathKind kind = PathKind::none;
    PathLifecycle lifecycle = PathLifecycle::stopped;
    std::optional<std::uint32_t> rtt_us;
    std::uint64_t sent_bytes = 0, received_bytes = 0;
    PathReason reason = PathReason::awaiting_relay;
    core::ErrorCode error = core::ErrorCode::none;
    std::uint64_t sent_packets = 0, received_packets = 0, dropped_packets = 0;
    std::optional<std::uint64_t> lost_packets = std::nullopt;
    std::optional<std::uint64_t> retransmitted_packets = std::nullopt;
    std::optional<std::uint64_t> retransmittable_packets = std::nullopt;
    std::uint32_t reconnects = 0;
    bool operator==(const PathStatus&) const = default;
};
struct PeerPathStatus {
    NetworkId network_id{};
    auth::DeviceId device_id{};
    std::uint32_t virtual_ipv4 = 0;
    PathStatus path;
    bool operator==(const PeerPathStatus&) const = default;
};
struct PathDiagnostics {
    PathStatus relay;
    std::vector<PeerPathStatus> peers;
    bool truncated = false;
    bool operator==(const PathDiagnostics&) const = default;
};
inline constexpr std::size_t max_diagnostic_peers = 256;
[[nodiscard]] std::vector<std::byte> encode_path_diagnostics(const PathDiagnostics& value);
[[nodiscard]] PathDiagnostics decode_path_diagnostics(std::span<const std::byte> value);
[[nodiscard]] std::string_view path_kind_name(PathKind kind) noexcept;
[[nodiscard]] std::string_view path_reason_name(PathReason reason) noexcept;
}
