#include "lanlink/protocol/codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lanlink::protocol {
namespace {

template <typename Integer>
void append_integer(std::vector<std::byte>& output, const Integer value) {
    static_assert(std::is_unsigned_v<Integer>);

    for (std::size_t index = sizeof(Integer); index > 0; --index) {
        const auto shift = static_cast<unsigned>((index - 1) * 8);
        output.push_back(std::byte{static_cast<unsigned char>((value >> shift) & 0xffU)});
    }
}

template <typename Integer>
Integer read_integer(const std::span<const std::byte> bytes, const std::size_t offset) {
    static_assert(std::is_unsigned_v<Integer>);
    Integer result = 0;

    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        result = static_cast<Integer>((result << 8U) |
                                      std::to_integer<unsigned char>(bytes[offset + index]));
    }

    return result;
}

}

bool is_known_message_type(const MessageType type) noexcept {
    switch (type) {
        case MessageType::client_hello:
        case MessageType::server_hello:
        case MessageType::heartbeat:
        case MessageType::heartbeat_ack:
        case MessageType::disconnect:
        case MessageType::client_auth:
        case MessageType::auth_result:
        case MessageType::network_create_request:
        case MessageType::network_list_request:
        case MessageType::network_join_request:
        case MessageType::network_leave_request:
        case MessageType::network_invite_request:
        case MessageType::network_approve_request:
        case MessageType::network_kick_request:
        case MessageType::network_peer_state_request:
        case MessageType::network_operation_result:
        case MessageType::network_list_result:
        case MessageType::network_event:
        case MessageType::network_peer_state_result:
        case MessageType::network_peer_state_update:
        case MessageType::network_peer_state_revoked:
        case MessageType::error:
            return true;
    }

    return false;
}

std::string_view message_type_name(const MessageType type) noexcept {
    switch (type) {
        case MessageType::client_hello:
            return "client_hello";
        case MessageType::server_hello:
            return "server_hello";
        case MessageType::heartbeat:
            return "heartbeat";
        case MessageType::heartbeat_ack:
            return "heartbeat_ack";
        case MessageType::disconnect:
            return "disconnect";
        case MessageType::client_auth:
            return "client_auth";
        case MessageType::auth_result:
            return "auth_result";
        case MessageType::network_create_request:
            return "network_create_request";
        case MessageType::network_list_request:
            return "network_list_request";
        case MessageType::network_join_request:
            return "network_join_request";
        case MessageType::network_leave_request:
            return "network_leave_request";
        case MessageType::network_invite_request:
            return "network_invite_request";
        case MessageType::network_approve_request:
            return "network_approve_request";
        case MessageType::network_kick_request:
            return "network_kick_request";
        case MessageType::network_peer_state_request:
            return "network_peer_state_request";
        case MessageType::network_operation_result:
            return "network_operation_result";
        case MessageType::network_list_result:
            return "network_list_result";
        case MessageType::network_event:
            return "network_event";
        case MessageType::network_peer_state_result:
            return "network_peer_state_result";
        case MessageType::network_peer_state_update:
            return "network_peer_state_update";
        case MessageType::network_peer_state_revoked:
            return "network_peer_state_revoked";
        case MessageType::error:
            return "error";
    }

    return "unknown";
}

std::vector<std::byte> encode_frame(const Frame& frame) {
    if (!is_known_message_type(frame.type)) {
        throw std::invalid_argument("unknown message type");
    }

    if (frame.payload.size() > max_payload_size ||
        frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("protocol payload is too large");
    }

    std::vector<std::byte> output;
    output.reserve(frame_header_size + frame.payload.size());
    output.insert(output.end(), frame_magic.begin(), frame_magic.end());
    append_integer(output, protocol_version);
    append_integer(output, static_cast<std::uint16_t>(frame.type));
    append_integer(output, frame.request_id);
    append_integer(output, static_cast<std::uint32_t>(frame.payload.size()));
    output.insert(output.end(), frame.payload.begin(), frame.payload.end());
    return output;
}

DecodeResult decode_frame(const std::span<const std::byte> bytes) {
    if (bytes.size() < frame_header_size) {
        return {DecodeStatus::need_more_data, std::nullopt, 0, frame_header_size};
    }

    if (!std::equal(frame_magic.begin(), frame_magic.end(), bytes.begin())) {
        return {DecodeStatus::invalid_magic, std::nullopt, 0, 0};
    }

    const auto version = read_integer<std::uint16_t>(bytes, 4);

    if (version != protocol_version) {
        return {DecodeStatus::unsupported_version, std::nullopt, 0, 0};
    }

    const auto raw_type = read_integer<std::uint16_t>(bytes, 6);
    const auto type = static_cast<MessageType>(raw_type);

    if (!is_known_message_type(type)) {
        return {DecodeStatus::unknown_message_type, std::nullopt, 0, 0};
    }

    const auto request_id = read_integer<std::uint32_t>(bytes, 8);
    const auto payload_size = read_integer<std::uint32_t>(bytes, 12);

    if (payload_size > max_payload_size) {
        return {DecodeStatus::payload_too_large, std::nullopt, 0, 0};
    }

    const auto total_size = frame_header_size + static_cast<std::size_t>(payload_size);

    if (bytes.size() < total_size) {
        return {DecodeStatus::need_more_data, std::nullopt, 0, total_size};
    }

    Frame frame;
    frame.type = type;
    frame.request_id = request_id;
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(frame_header_size),
                         bytes.begin() + static_cast<std::ptrdiff_t>(total_size));

    return {DecodeStatus::complete, std::move(frame), total_size, total_size};
}

std::string_view decode_status_name(const DecodeStatus status) noexcept {
    switch (status) {
        case DecodeStatus::complete:
            return "complete";
        case DecodeStatus::need_more_data:
            return "need_more_data";
        case DecodeStatus::invalid_magic:
            return "invalid_magic";
        case DecodeStatus::unsupported_version:
            return "unsupported_version";
        case DecodeStatus::unknown_message_type:
            return "unknown_message_type";
        case DecodeStatus::payload_too_large:
            return "payload_too_large";
    }

    return "unknown";
}

}
