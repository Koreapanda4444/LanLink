#include "lanlink/protocol/local_control.hpp"

#include <stdexcept>

namespace lanlink::protocol {
namespace {

constexpr std::byte magic[]{std::byte{'L'}, std::byte{'L'},
                            std::byte{'C'}, std::byte{'P'}};

bool valid_command(const LocalCommand command) {
    return command >= LocalCommand::status && command <= LocalCommand::stop;
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

}
