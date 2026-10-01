#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lanlink::protocol {

inline constexpr std::size_t max_local_control_payload = 512 * 1024;
inline constexpr std::size_t local_control_header_size = 12;

enum class LocalCommand : std::uint8_t {
    status = 1,
    list = 2,
    create = 3,
    join = 4,
    leave = 5,
    invite = 6,
    approve = 7,
    kick = 8,
    stop = 9,
    diagnostics = 10,
};

struct LocalDiagnostics {
    bool connected = false;
    bool authenticated = false;
    bool tcp_fallback = false;
    std::uint32_t active_networks = 0;
    std::optional<std::uint32_t> rtt_us;
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint32_t reconnects = 0;
    std::string last_error;

    bool operator==(const LocalDiagnostics&) const = default;
};

struct LocalMessage {
    LocalCommand command = LocalCommand::status;
    bool response = false;
    bool error = false;
    std::vector<std::byte> payload;

    bool operator==(const LocalMessage&) const = default;
};

[[nodiscard]] std::vector<std::byte> encode_local_message(const LocalMessage& message);
[[nodiscard]] LocalMessage decode_local_message(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_local_diagnostics(
    const LocalDiagnostics& diagnostics);
[[nodiscard]] LocalDiagnostics decode_local_diagnostics(
    std::span<const std::byte> payload);

}
