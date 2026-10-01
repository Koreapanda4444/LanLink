#include "lanlink/protocol/local_control.hpp"

#include <stdexcept>
#include <limits>

namespace lanlink::protocol {
namespace {

constexpr std::byte magic[]{std::byte{'L'}, std::byte{'L'},
                            std::byte{'C'}, std::byte{'P'}};

bool valid_command(const LocalCommand command) {
    return command >= LocalCommand::status && command <= LocalCommand::diagnostics;
}

}

std::vector<std::byte> encode_local_message(const LocalMessage& message) {
    if (!valid_command(message.command) || (message.error && !message.response) ||
        message.payload.size() > max_local_control_payload) {
        throw std::invalid_argument("invalid local control message");
    }
    std::vector<std::byte> bytes(local_control_header_size + message.payload.size());
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i] = magic[i];
    }
    bytes[4] = std::byte{1};
    bytes[5] = static_cast<std::byte>(message.command);
    bytes[6] = static_cast<std::byte>((message.response ? 1 : 0) |
                                      (message.error ? 2 : 0));
    const auto length = static_cast<std::uint32_t>(message.payload.size());
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[8 + i] = static_cast<std::byte>((length >> (i * 8)) & 0xff);
    }
    for (std::size_t i = 0; i < message.payload.size(); ++i) {
        bytes[local_control_header_size + i] = message.payload[i];
    }
    return bytes;
}

LocalMessage decode_local_message(const std::span<const std::byte> bytes) {
    if (bytes.size() < local_control_header_size || bytes[4] != std::byte{1} ||
        bytes[7] != std::byte{0} ||
        (std::to_integer<unsigned>(bytes[6]) & ~3U) != 0) {
        throw std::invalid_argument("invalid local control header");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (bytes[i] != magic[i]) {
            throw std::invalid_argument("invalid local control magic");
        }
    }
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        length |= std::to_integer<std::uint32_t>(bytes[8 + i]) << (i * 8);
    }
    const auto command = static_cast<LocalCommand>(std::to_integer<unsigned>(bytes[5]));
    const auto flags = std::to_integer<unsigned>(bytes[6]);
    if (!valid_command(command) || (flags & 2U && !(flags & 1U)) ||
        length > max_local_control_payload ||
        bytes.size() != local_control_header_size + length) {
        throw std::invalid_argument("invalid local control message");
    }
    return {command, (flags & 1U) != 0, (flags & 2U) != 0,
            {bytes.begin() + local_control_header_size, bytes.end()}};
}

std::vector<std::byte> encode_local_diagnostics(
    const LocalDiagnostics& diagnostics) {
    if (diagnostics.last_error.size() > 512) {
        throw std::invalid_argument("local diagnostic error is too long");
    }
    std::vector<std::byte> output;
    output.reserve(34 + diagnostics.last_error.size());
    output.push_back(static_cast<std::byte>(diagnostics.connected));
    output.push_back(static_cast<std::byte>(diagnostics.authenticated));
    output.push_back(static_cast<std::byte>(diagnostics.tcp_fallback));
    output.push_back(std::byte{0});
    const auto append = [&](const std::uint64_t value, const std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            output.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
        }
    };
    append(diagnostics.active_networks, 4);
    append(diagnostics.rtt_us.value_or(std::numeric_limits<std::uint32_t>::max()), 4);
    append(diagnostics.sent_bytes, 8);
    append(diagnostics.received_bytes, 8);
    append(diagnostics.reconnects, 4);
    append(diagnostics.last_error.size(), 2);
    for (const auto ch : diagnostics.last_error) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return output;
}

LocalDiagnostics decode_local_diagnostics(const std::span<const std::byte> payload) {
    if (payload.size() < 34 || payload[0] > std::byte{1} ||
        payload[1] > std::byte{1} || payload[2] > std::byte{1} ||
        payload[3] != std::byte{0}) {
        throw std::invalid_argument("invalid local diagnostics");
    }
    const auto read = [&](const std::size_t offset, const std::size_t count) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            value |= std::to_integer<std::uint64_t>(payload[offset + i]) << (8 * i);
        }
        return value;
    };
    const auto error_size = read(32, 2);
    if (error_size > 512 || payload.size() != 34 + error_size) {
        throw std::invalid_argument("invalid local diagnostic error size");
    }
    LocalDiagnostics result;
    result.connected = payload[0] == std::byte{1};
    result.authenticated = payload[1] == std::byte{1};
    result.tcp_fallback = payload[2] == std::byte{1};
    result.active_networks = static_cast<std::uint32_t>(read(4, 4));
    const auto rtt = static_cast<std::uint32_t>(read(8, 4));
    if (rtt != std::numeric_limits<std::uint32_t>::max()) result.rtt_us = rtt;
    result.sent_bytes = read(12, 8);
    result.received_bytes = read(20, 8);
    result.reconnects = static_cast<std::uint32_t>(read(28, 4));
    result.last_error.assign(reinterpret_cast<const char*>(payload.data() + 34),
                             static_cast<std::size_t>(error_size));
    return result;
}

}
