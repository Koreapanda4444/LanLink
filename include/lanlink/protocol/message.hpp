#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace lanlink::protocol {

inline constexpr std::array<std::byte, 4> frame_magic{
    std::byte{0x4c},
    std::byte{0x4e},
    std::byte{0x4c},
    std::byte{0x4b},
};
inline constexpr std::uint16_t protocol_version = 3;
inline constexpr std::string_view protocol_alpn = "lanlink/3";
inline constexpr std::size_t frame_header_size = 16;
inline constexpr std::uint32_t max_payload_size = 1024U * 1024U;

enum class MessageType : std::uint16_t {
    client_hello = 0x0001,
    server_hello = 0x0002,
    heartbeat = 0x0003,
    heartbeat_ack = 0x0004,
    disconnect = 0x0005,
    client_auth = 0x0006,
    auth_result = 0x0007,
    network_create_request = 0x0100,
    network_list_request = 0x0101,
    network_join_request = 0x0102,
    network_leave_request = 0x0103,
    network_invite_request = 0x0104,
    network_approve_request = 0x0105,
    network_kick_request = 0x0106,
    network_peer_state_request = 0x0107,
    network_operation_result = 0x0180,
    network_list_result = 0x0181,
    network_event = 0x0182,
    network_peer_state_result = 0x0183,
    network_peer_state_update = 0x0184,
    network_peer_state_revoked = 0x0185,
    error = 0x00ff,
};

struct Frame {
    MessageType type = MessageType::client_hello;
    std::uint32_t request_id = 0;
    std::vector<std::byte> payload;

    bool operator==(const Frame&) const = default;
};

[[nodiscard]] bool is_known_message_type(MessageType type) noexcept;
[[nodiscard]] std::string_view message_type_name(MessageType type) noexcept;

}
