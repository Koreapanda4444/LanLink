#include "lanlink/protocol/capabilities.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace lanlink::protocol {
namespace {
constexpr std::array magic{std::byte{'L'}, std::byte{'C'}, std::byte{'A'}, std::byte{'P'}};
void append(std::vector<std::byte>& out, std::uint64_t value, unsigned count) {
    for (unsigned i = count; i > 0; --i) out.push_back(static_cast<std::byte>((value >> (8 * (i - 1))) & 255));
}
std::uint64_t read(std::span<const std::byte> in, std::size_t offset, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value = (value << 8) | std::to_integer<unsigned>(in[offset + i]);
    return value;
}
void validate(const CapabilityOffer& offer) {
    if (!offer.minimum_version || offer.minimum_version > offer.maximum_version)
        throw std::invalid_argument("invalid protocol version range");
}
}

bool is_capability_payload(std::span<const std::byte> payload) noexcept {
    return payload.size() >= magic.size() && std::equal(magic.begin(), magic.end(), payload.begin());
}

std::vector<std::byte> encode_capability_offer(const CapabilityOffer& offer) {
    validate(offer);
    std::vector<std::byte> out(magic.begin(), magic.end());
    out.push_back(std::byte{1}); out.push_back(std::byte{0});
    append(out, offer.minimum_version, 2); append(out, offer.maximum_version, 2); append(out, offer.features, 8);
    return out;
}

CapabilityOffer decode_capability_offer(std::span<const std::byte> payload) {
    if (!is_capability_payload(payload) || payload.size() != 18 || payload[4] != std::byte{1} || payload[5] != std::byte{0})
        throw std::invalid_argument("invalid capability offer");
    CapabilityOffer result{static_cast<std::uint16_t>(read(payload, 6, 2)),
                           static_cast<std::uint16_t>(read(payload, 8, 2)), read(payload, 10, 8)};
    validate(result); return result;
}

NegotiatedCapabilities negotiate_capabilities(const CapabilityOffer& local, const CapabilityOffer& remote) {
    validate(local); validate(remote);
    const auto low = std::max(local.minimum_version, remote.minimum_version);
    const auto high = std::min(local.maximum_version, remote.maximum_version);
    if (low > high) return {};
    return {high, local.features & remote.features, false};
}

std::vector<std::byte> encode_capability_selection(const NegotiatedCapabilities& selection) {
    if (!selection.wire_version || (selection.legacy && selection.features))
        throw std::invalid_argument("invalid capability selection");
    std::vector<std::byte> out(magic.begin(), magic.end());
    out.push_back(std::byte{1}); out.push_back(selection.legacy ? std::byte{2} : std::byte{1});
    append(out, selection.wire_version, 2); append(out, selection.features, 8); return out;
}

NegotiatedCapabilities decode_capability_selection(std::span<const std::byte> payload) {
    if (!is_capability_payload(payload) || payload.size() != 16 || payload[4] != std::byte{1} ||
        (payload[5] != std::byte{1} && payload[5] != std::byte{2})) throw std::invalid_argument("invalid capability selection");
    NegotiatedCapabilities result{static_cast<std::uint16_t>(read(payload, 6, 2)), read(payload, 8, 8), payload[5] == std::byte{2}};
    static_cast<void>(encode_capability_selection(result)); return result;
}

}
