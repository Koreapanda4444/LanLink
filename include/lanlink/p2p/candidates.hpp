#pragma once
#include "lanlink/protocol/network_messages.hpp"
#include <span>

namespace lanlink::p2p {
enum class CandidateKind : std::uint8_t { host = 1, server_reflexive = 2 };
struct Endpoint {
    std::uint32_t ipv4 = 0; // host byte order; numeric IPv4 only in schema 1
    std::uint16_t port = 0;
    auto operator<=>(const Endpoint&) const = default;
};
struct Candidate {
    Endpoint endpoint;
    CandidateKind kind = CandidateKind::host;
    std::uint32_t priority = 0;
    bool operator==(const Candidate&) const = default;
};
inline constexpr std::size_t max_candidates = 8;
struct CandidateExchange {
    protocol::NetworkId network_id{};
    auth::DeviceId sender{}, recipient{};
    std::array<std::byte, 32> sender_session{}, recipient_session{};
    std::uint64_t key_epoch = 0, generation = 0;
    std::uint32_t lifetime_ms = 30'000;
    std::vector<Candidate> candidates;
    bool operator==(const CandidateExchange&) const = default;
};
[[nodiscard]] bool usable_endpoint(Endpoint endpoint, bool allow_loopback = true) noexcept;
[[nodiscard]] std::vector<std::byte> encode_candidates(const CandidateExchange& value);
[[nodiscard]] CandidateExchange decode_candidates(std::span<const std::byte> bytes);
}
