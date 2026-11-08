#pragma once
#include "lanlink/protocol/network_messages.hpp"
#include <string>
#include <span>
#include <vector>
namespace lanlink::protocol {
struct ServerProfile {
    std::string id, name, host;
    std::uint16_t port = 4433;
    // SHA-256 of DER SubjectPublicKeyInfo. Empty means normal OS/CA trust.
    std::string public_key_pin;
    bool has_credential = false;
    bool operator==(const ServerProfile&) const = default;
};
enum class ServerAction : std::uint8_t { list, save, remove, connect, disconnect, enroll };
struct ServerRequest {
    ServerAction action = ServerAction::list;
    ServerProfile profile;
    std::string credential, registration_code, device_label;
};
struct ServerState {
    std::uint64_t revision = 0;
    std::string selected_id;
    std::vector<ServerProfile> profiles;
    bool operator==(const ServerState&) const = default;
};
struct ServerInvitation {
    ServerProfile server;
    std::string registration_code;
    NetworkId network_id{};
    std::string network_invitation;
    bool operator==(const ServerInvitation&) const = default;
};
void validate_server_profile(const ServerProfile& profile, bool require_id = false);
std::vector<std::byte> encode_server_request(const ServerRequest& request);
ServerRequest decode_server_request(std::span<const std::byte> bytes);
std::vector<std::byte> encode_server_state(const ServerState& state);
ServerState decode_server_state(std::span<const std::byte> bytes);
std::string make_server_invitation(const ServerInvitation& invitation);
ServerInvitation parse_server_invitation(std::string_view uri);
}
