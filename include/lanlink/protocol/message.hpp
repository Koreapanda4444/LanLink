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
inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::string_view protocol_alpn = "lanlink/1";
inline constexpr std::size_t frame_header_size = 16;
inline constexpr std::uint32_t max_payload_size = 1024U * 1024U;

enum class MessageType : std::uint16_t {
    client_hello = 0x0001,
    server_hello = 0x0002,
    heartbeat = 0x0003,
    heartbeat_ack = 0x0004,
    disconnect = 0x0005,
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
