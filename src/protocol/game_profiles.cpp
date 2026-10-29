#include "lanlink/protocol/game_profiles.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace lanlink::protocol {
namespace {
constexpr std::array profiles{GameProfile{
    "teeworlds075", "Teeworlds", "0.7.5", 8303, 8310,
    "Allow Teeworlds UDP on the LanLink interface. Try LAN refresh, then the host virtual IPv4:port. "
    "Custom server ports must be entered explicitly. Dedicated-server query verified; graphical gameplay remains unverified.",
    "teeworlds/teeworlds@4fc25a17fef3e6c2bf4d52b0421e0d69ecaa1e79"}};
void put(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = std::byte{static_cast<unsigned char>(value >> ((3 - index) * 8U))};
}
std::uint32_t get(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + index]);
    return value;
}
constexpr std::array info_header{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
    std::byte{'i'}, std::byte{'n'}, std::byte{'f'}, std::byte{'3'}};
}
std::span<const GameProfile> game_profiles() noexcept { return profiles; }

std::vector<std::byte> teeworlds_token_request(const std::uint32_t client_token) {
    if (client_token == 0xffffffffU) throw std::invalid_argument("invalid game query token");
    std::vector<std::byte> bytes(520);
    bytes[0] = std::byte{4}; put(bytes, 3, 0xffffffffU);
    bytes[7] = std::byte{5}; put(bytes, 8, client_token);
    return bytes;
}
std::optional<std::uint32_t> teeworlds_server_token(std::span<const std::byte> bytes, const std::uint32_t client_token) {
    if (bytes.size() != 12 || bytes[0] != std::byte{4} || bytes[1] != std::byte{0} ||
        bytes[2] != std::byte{0} || bytes[7] != std::byte{5} || get(bytes, 3) != client_token ||
        get(bytes, 8) == 0xffffffffU) return std::nullopt;
    return get(bytes, 8);
}
std::vector<std::byte> teeworlds_info_request(const std::uint32_t client_token,
    const std::uint32_t server_token, const std::uint8_t challenge) {
    if (client_token == 0xffffffffU || server_token == 0xffffffffU || challenge > 63)
        throw std::invalid_argument("invalid game query challenge");
    std::vector<std::byte> bytes(18);
    bytes[0] = std::byte{0x21}; put(bytes, 1, server_token); put(bytes, 5, client_token);
    std::copy(info_header.begin(), info_header.end(), bytes.begin() + 9);
    bytes[13] = std::byte{'g'}; bytes[14] = std::byte{'i'}; bytes[15] = std::byte{'e'};
    bytes[17] = std::byte{challenge};
    return bytes;
}
std::optional<GameServerInfo> teeworlds_server_info(std::span<const std::byte> bytes,
    const std::uint32_t client_token, const std::uint8_t challenge) {
    if (bytes.size() < 20 || bytes.size() > 1400 || bytes[0] != std::byte{0x21} || get(bytes, 1) != client_token ||
        !std::equal(info_header.begin(), info_header.end(), bytes.begin() + 9) ||
        challenge > 63 || bytes[17] != std::byte{challenge}) return std::nullopt;
    std::size_t offset = 18;
    const auto string = [&](std::size_t maximum) -> std::optional<std::string> {
        std::string value;
        while (offset < bytes.size() && value.size() < maximum) {
            const auto byte = std::to_integer<unsigned char>(bytes[offset++]);
            if (byte == 0) return value;
            value += byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?';
        }
        return std::nullopt;
    };
    const auto version = string(32), name = string(64);
    if (!version || *version != "0.7.5" || !name || name->empty()) return std::nullopt;
    return GameServerInfo{*version, *name};
}
}
