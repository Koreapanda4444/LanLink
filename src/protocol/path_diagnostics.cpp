#include "lanlink/protocol/path_diagnostics.hpp"
#include "lanlink/protocol/binary.hpp"

namespace lanlink::protocol {
namespace {
void validate(const PathStatus& p) {
    if (p.kind > PathKind::direct_quic || p.lifecycle > PathLifecycle::failed ||
        p.reason > PathReason::stopped || p.error > core::ErrorCode::revoked)
        throw std::invalid_argument("invalid path diagnostic state");
}
void write_path(BinaryWriter& w, const PathStatus& p) {
    validate(p);
    w.put(static_cast<std::uint8_t>(p.kind)); w.put(static_cast<std::uint8_t>(p.lifecycle));
    w.put(static_cast<std::uint8_t>(p.reason)); w.put(static_cast<std::uint8_t>(p.error));
    w.put(static_cast<std::uint8_t>((p.rtt_us ? 1 : 0) | (p.lost_packets ? 2 : 0) |
        (p.retransmitted_packets ? 4 : 0) | (p.retransmittable_packets ? 8 : 0)));
    w.put(p.rtt_us.value_or(0)); w.put(p.sent_bytes); w.put(p.received_bytes);
    w.put(p.sent_packets); w.put(p.received_packets); w.put(p.dropped_packets);
    w.put(p.lost_packets.value_or(0)); w.put(p.retransmitted_packets.value_or(0));
    w.put(p.retransmittable_packets.value_or(0)); w.put(p.reconnects);
}
PathStatus read_path(BinaryReader& r) {
    PathStatus p;
    p.kind = static_cast<PathKind>(r.get<std::uint8_t>()); p.lifecycle = static_cast<PathLifecycle>(r.get<std::uint8_t>());
    p.reason = static_cast<PathReason>(r.get<std::uint8_t>()); p.error = static_cast<core::ErrorCode>(r.get<std::uint8_t>());
    const auto flags = r.get<std::uint8_t>();
    if (flags & ~15U) throw std::invalid_argument("invalid path diagnostic flags");
    const auto rtt = r.get<std::uint32_t>(); if (flags & 1) p.rtt_us = rtt;
    p.sent_bytes = r.get<std::uint64_t>(); p.received_bytes = r.get<std::uint64_t>();
    p.sent_packets = r.get<std::uint64_t>(); p.received_packets = r.get<std::uint64_t>(); p.dropped_packets = r.get<std::uint64_t>();
    const auto lost = r.get<std::uint64_t>(), retransmitted = r.get<std::uint64_t>(), retransmittable = r.get<std::uint64_t>();
    if (flags & 2) p.lost_packets = lost;
    if (flags & 4) p.retransmitted_packets = retransmitted;
    if (flags & 8) p.retransmittable_packets = retransmittable;
    if ((!(flags & 1) && rtt) || (!(flags & 2) && lost) || (!(flags & 4) && retransmitted) || (!(flags & 8) && retransmittable))
        throw std::invalid_argument("noncanonical path diagnostic value");
    p.reconnects = r.get<std::uint32_t>(); validate(p); return p;
}
}

std::vector<std::byte> encode_path_diagnostics(const PathDiagnostics& value) {
    if (value.peers.size() > max_diagnostic_peers) throw std::length_error("too many diagnostic peers");
    BinaryWriter w; w.put(std::uint32_t{0x4c4c5044}); w.put(std::uint8_t{1});
    w.put(static_cast<std::uint8_t>(value.truncated)); w.put(static_cast<std::uint16_t>(value.peers.size()));
    write_path(w, value.relay);
    for (const auto& peer : value.peers) {
        w.append(peer.network_id); w.append(peer.device_id); w.put(peer.virtual_ipv4); write_path(w, peer.path);
    }
    return std::move(w).finish();
}

PathDiagnostics decode_path_diagnostics(std::span<const std::byte> value) {
    BinaryReader r(value);
    if (r.get<std::uint32_t>() != 0x4c4c5044 || r.get<std::uint8_t>() != 1) throw std::invalid_argument("invalid path diagnostics header");
    const auto flags = r.get<std::uint8_t>(); const auto count = r.get<std::uint16_t>();
    if (flags > 1 || count > max_diagnostic_peers) throw std::invalid_argument("invalid path diagnostics size");
    PathDiagnostics result; result.truncated = flags != 0; result.relay = read_path(r);
    for (std::size_t i = 0; i < count; ++i) {
        PeerPathStatus p; p.network_id = r.array<network_id_size>(); p.device_id = r.array<auth::device_id_size>();
        p.virtual_ipv4 = r.get<std::uint32_t>(); p.path = read_path(r); result.peers.push_back(p);
    }
    r.finish(); return result;
}

std::string_view path_kind_name(PathKind kind) noexcept {
    switch (kind) {
    case PathKind::none: return "Unavailable";
    case PathKind::relay_quic: return "Relay QUIC";
    case PathKind::relay_tls: return "Relay TLS/TCP";
    case PathKind::direct_quic: return "Direct QUIC";
    }
    return "Unavailable";
}
std::string_view path_reason_name(PathReason reason) noexcept {
    switch (reason) {
    case PathReason::awaiting_relay: return "Waiting for relay authentication";
    case PathReason::relayed: return "Using the relay";
    case PathReason::direct_ready: return "Authenticated direct path selected";
    case PathReason::direct_unreachable: return "Direct path unavailable";
    case PathReason::udp_blocked: return "Relay UDP unavailable; using TLS/TCP";
    case PathReason::degraded: return "Direct path quality degraded";
    case PathReason::revalidating: return "Network path is being revalidated";
    case PathReason::revoked: return "Network membership or key changed";
    case PathReason::stopped: return "Connection stopped";
    }
    return "Connection stopped";
}
}
