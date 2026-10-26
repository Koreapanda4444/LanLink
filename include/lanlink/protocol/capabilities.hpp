#pragma once

#include "lanlink/protocol/message.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace lanlink::protocol {

inline constexpr std::uint32_t capability_request_id = 0xffffffffU;
enum class Capability : std::uint64_t {
    negotiation = 1, structured_diagnostics = 2, peer_candidates = 4, direct_quic = 8,
};
inline constexpr std::uint64_t supported_capabilities = static_cast<std::uint64_t>(Capability::negotiation);

struct CapabilityOffer {
    std::uint16_t minimum_version = protocol_version;
    std::uint16_t maximum_version = protocol_version;
    std::uint64_t features = supported_capabilities;
    bool operator==(const CapabilityOffer&) const = default;
};

struct NegotiatedCapabilities {
    std::uint16_t wire_version = protocol_version;
    std::uint64_t features = 0;
    bool legacy = true;
    [[nodiscard]] bool supports(Capability capability) const noexcept {
        return !legacy && (features & static_cast<std::uint64_t>(capability)) != 0;
    }
    bool operator==(const NegotiatedCapabilities&) const = default;
};

[[nodiscard]] bool is_capability_payload(std::span<const std::byte> payload) noexcept;
[[nodiscard]] std::vector<std::byte> encode_capability_offer(const CapabilityOffer& offer);
[[nodiscard]] CapabilityOffer decode_capability_offer(std::span<const std::byte> payload);
[[nodiscard]] NegotiatedCapabilities negotiate_capabilities(const CapabilityOffer& local, const CapabilityOffer& remote);
[[nodiscard]] std::vector<std::byte> encode_capability_selection(const NegotiatedCapabilities& selection);
[[nodiscard]] NegotiatedCapabilities decode_capability_selection(std::span<const std::byte> payload);

}
