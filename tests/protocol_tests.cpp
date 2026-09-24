#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/message.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view name) {
    if (!condition) {
        std::cerr << "failed: " << name << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string_view name) {
    try {
        function();
        expect(false, name);
    } catch (const std::exception&) {
    }
}

std::vector<std::byte> make_bytes(const std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());

    for (const auto value : values) {
        result.push_back(std::byte{static_cast<unsigned char>(value)});
    }

    return result;
}

void test_constants() {
    using namespace lanlink::protocol;

    expect(protocol_version == 2, "protocol version");
    expect(protocol_alpn == "lanlink/2", "protocol alpn");
    expect(frame_header_size == 16, "frame header size");
    expect(message_type_name(MessageType::client_hello) == "client_hello", "client hello name");
    expect(message_type_name(MessageType::client_auth) == "client_auth", "client auth name");
    expect(message_type_name(MessageType::auth_result) == "auth_result", "auth result name");
    expect(message_type_name(MessageType::network_create_request) ==
               "network_create_request",
           "network create name");
    expect(message_type_name(MessageType::network_operation_result) ==
               "network_operation_result",
           "network result name");
    expect(message_type_name(MessageType::network_event) == "network_event",
           "network event name");
    expect(message_type_name(MessageType::error) == "error", "error name");
    expect(message_type_name(static_cast<MessageType>(0x7777)) == "unknown", "unknown type name");
    expect(is_known_message_type(MessageType::heartbeat), "known message type");
    expect(!is_known_message_type(static_cast<MessageType>(0x7777)), "unknown message type");
}

void test_golden_frame() {
    using namespace lanlink::protocol;

    Frame frame;
    frame.type = MessageType::heartbeat;
    frame.request_id = 0x01020304U;
    frame.payload = make_bytes({0xde, 0xad});

    const auto encoded = encode_frame(frame);
    const auto expected = make_bytes({
        0x4c, 0x4e, 0x4c, 0x4b,
        0x00, 0x02,
        0x00, 0x03,
        0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x02,
        0xde, 0xad,
    });

    expect(encoded == expected, "golden frame bytes");

    const auto decoded = decode_frame(encoded);
    expect(decoded.complete(), "golden frame complete");
    expect(decoded.frame == frame, "golden frame round trip");
    expect(decoded.consumed == encoded.size(), "golden frame consumed");
    expect(decoded.required == encoded.size(), "golden frame required");
}

void test_message_round_trips() {
    using namespace lanlink::protocol;

    constexpr std::array types{
        MessageType::client_hello,
        MessageType::server_hello,
        MessageType::heartbeat,
        MessageType::heartbeat_ack,
        MessageType::disconnect,
        MessageType::client_auth,
        MessageType::auth_result,
        MessageType::network_create_request,
        MessageType::network_list_request,
        MessageType::network_join_request,
        MessageType::network_leave_request,
        MessageType::network_invite_request,
        MessageType::network_approve_request,
        MessageType::network_kick_request,
        MessageType::network_operation_result,
        MessageType::network_list_result,
        MessageType::network_event,
        MessageType::error,
    };

    for (std::size_t index = 0; index < types.size(); ++index) {
        Frame frame;
        frame.type = types[index];
        frame.request_id = static_cast<std::uint32_t>(index + 1);

        for (unsigned int value = 0; value < 256; ++value) {
            frame.payload.push_back(std::byte{static_cast<unsigned char>(value)});
        }

        const auto encoded = encode_frame(frame);
        const auto decoded = decode_frame(encoded);
        expect(decoded.complete(), "message round trip status");
        expect(decoded.frame == frame, "message round trip frame");
    }
}

void test_concatenated_frames() {
    using namespace lanlink::protocol;

    Frame first;
    first.type = MessageType::client_hello;
    first.request_id = 7;
    first.payload = make_bytes({1, 2, 3});

    Frame second;
    second.type = MessageType::server_hello;
    second.request_id = 7;
    second.payload = make_bytes({4, 5});

    const auto first_bytes = encode_frame(first);
    const auto second_bytes = encode_frame(second);
    auto stream = first_bytes;
    stream.insert(stream.end(), second_bytes.begin(), second_bytes.end());

    const auto first_result = decode_frame(stream);
    expect(first_result.frame == first, "first concatenated frame");
    expect(first_result.consumed == first_bytes.size(), "first concatenated size");

    const auto remaining = std::span<const std::byte>(stream).subspan(first_result.consumed);
    const auto second_result = decode_frame(remaining);
    expect(second_result.frame == second, "second concatenated frame");
    expect(second_result.consumed == second_bytes.size(), "second concatenated size");
}

void test_truncated_frame() {
    using namespace lanlink::protocol;

    Frame frame;
    frame.type = MessageType::heartbeat;
    frame.payload = make_bytes({1, 2, 3, 4, 5});
    const auto encoded = encode_frame(frame);

    for (std::size_t size = 0; size < encoded.size(); ++size) {
        const auto result = decode_frame(std::span<const std::byte>(encoded.data(), size));
        expect(result.status == DecodeStatus::need_more_data, "truncated status");
        expect(!result.frame.has_value(), "truncated frame absent");
        expect(result.consumed == 0, "truncated consumed");
        expect(result.required == (size < frame_header_size ? frame_header_size : encoded.size()),
               "truncated required");
    }
}

void test_malformed_frames() {
    using namespace lanlink::protocol;

    Frame frame;
    frame.type = MessageType::heartbeat;
    frame.payload = make_bytes({1});
    const auto valid = encode_frame(frame);

    auto invalid_magic = valid;
    invalid_magic[0] = std::byte{0};
    expect(decode_frame(invalid_magic).status == DecodeStatus::invalid_magic, "invalid magic");

    auto unsupported_version = valid;
    unsupported_version[4] = std::byte{0};
    unsupported_version[5] = std::byte{3};
    expect(decode_frame(unsupported_version).status == DecodeStatus::unsupported_version,
           "unsupported version");

    auto unknown_type = valid;
    unknown_type[6] = std::byte{0x77};
    unknown_type[7] = std::byte{0x77};
    expect(decode_frame(unknown_type).status == DecodeStatus::unknown_message_type,
           "unknown message type");

    auto oversized = valid;
    oversized[12] = std::byte{0x00};
    oversized[13] = std::byte{0x10};
    oversized[14] = std::byte{0x00};
    oversized[15] = std::byte{0x01};
    expect(decode_frame(oversized).status == DecodeStatus::payload_too_large,
           "oversized wire payload");

    expect_error([] {
        lanlink::protocol::Frame unknown;
        unknown.type = static_cast<lanlink::protocol::MessageType>(0x7777);
        static_cast<void>(lanlink::protocol::encode_frame(unknown));
    }, "encode unknown type");

    expect_error([] {
        lanlink::protocol::Frame oversized_frame;
        oversized_frame.payload.resize(lanlink::protocol::max_payload_size + 1U);
        static_cast<void>(lanlink::protocol::encode_frame(oversized_frame));
    }, "encode oversized payload");
}

void test_payload_limit() {
    using namespace lanlink::protocol;

    Frame frame;
    frame.type = MessageType::server_hello;
    frame.request_id = std::numeric_limits<std::uint32_t>::max();
    frame.payload.resize(max_payload_size, std::byte{0x5a});

    const auto encoded = encode_frame(frame);
    const auto decoded = decode_frame(encoded);
    expect(encoded.size() == frame_header_size + max_payload_size, "maximum encoded size");
    expect(decoded.complete(), "maximum payload status");
    expect(decoded.frame == frame, "maximum payload round trip");
}

void test_generated_frames() {
    using namespace lanlink::protocol;

    constexpr std::array types{
        MessageType::client_hello,
        MessageType::server_hello,
        MessageType::heartbeat,
        MessageType::heartbeat_ack,
        MessageType::disconnect,
        MessageType::client_auth,
        MessageType::auth_result,
        MessageType::network_create_request,
        MessageType::network_list_request,
        MessageType::network_join_request,
        MessageType::network_leave_request,
        MessageType::network_invite_request,
        MessageType::network_approve_request,
        MessageType::network_kick_request,
        MessageType::network_operation_result,
        MessageType::network_list_result,
        MessageType::network_event,
        MessageType::error,
    };
    std::uint32_t state = 0x13579bdfU;

    for (std::uint32_t iteration = 0; iteration < 500; ++iteration) {
        state = state * 1664525U + 1013904223U;
        Frame frame;
        frame.type = types[state % types.size()];
        frame.request_id = state;
        state = state * 1664525U + 1013904223U;
        const auto size = static_cast<std::size_t>(state % 2048U);
        frame.payload.reserve(size);

        for (std::size_t index = 0; index < size; ++index) {
            state = state * 1664525U + 1013904223U;
            frame.payload.push_back(std::byte{static_cast<unsigned char>(state >> 24U)});
        }

        const auto encoded = encode_frame(frame);
        const auto decoded = decode_frame(encoded);
        expect(decoded.complete(), "generated frame status");
        expect(decoded.frame == frame, "generated frame round trip");
    }
}

void test_status_names() {
    using namespace lanlink::protocol;

    expect(decode_status_name(DecodeStatus::complete) == "complete", "complete status name");
    expect(decode_status_name(DecodeStatus::need_more_data) == "need_more_data",
           "incomplete status name");
    expect(decode_status_name(DecodeStatus::invalid_magic) == "invalid_magic",
           "magic status name");
    expect(decode_status_name(DecodeStatus::unsupported_version) == "unsupported_version",
           "version status name");
    expect(decode_status_name(DecodeStatus::unknown_message_type) == "unknown_message_type",
           "type status name");
    expect(decode_status_name(DecodeStatus::payload_too_large) == "payload_too_large",
           "payload status name");
}

}

int main() {
    test_constants();
    test_golden_frame();
    test_message_round_trips();
    test_concatenated_frames();
    test_truncated_frame();
    test_malformed_frames();
    test_payload_limit();
    test_generated_frames();
    test_status_names();

    if (failures != 0) {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "all protocol tests passed\n";
    return 0;
}
