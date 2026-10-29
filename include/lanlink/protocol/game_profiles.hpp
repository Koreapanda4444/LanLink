#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanlink::protocol {

struct GameProfile {
    std::string_view id, name, version;
    std::uint16_t first_port, last_port;
    std::string_view guidance, evidence;
};
[[nodiscard]] std::span<const GameProfile> game_profiles() noexcept;
struct GameServerInfo { std::string version, name; };
[[nodiscard]] std::vector<std::byte> teeworlds_token_request(std::uint32_t client_token);
[[nodiscard]] std::optional<std::uint32_t> teeworlds_server_token(std::span<const std::byte> packet, std::uint32_t client_token);
[[nodiscard]] std::vector<std::byte> teeworlds_info_request(std::uint32_t client_token, std::uint32_t server_token, std::uint8_t challenge);
[[nodiscard]] std::optional<GameServerInfo> teeworlds_server_info(std::span<const std::byte> packet, std::uint32_t client_token, std::uint8_t challenge);

}
