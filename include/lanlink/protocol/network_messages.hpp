#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanlink::protocol {

inline constexpr std::size_t network_id_size = 16;
inline constexpr std::size_t network_device_id_size = 32;
inline constexpr std::size_t max_network_name_size = 128;
inline constexpr std::size_t max_network_list_entries = 1024;

using NetworkId = std::array<std::byte, network_id_size>;
using NetworkDeviceId = std::array<std::byte, network_device_id_size>;

enum class NetworkRole : std::uint8_t {
    member = 0,
    owner = 1,
};

enum class NetworkOperation : std::uint8_t {
    create = 1,
    list = 2,
    join = 3,
    leave = 4,
    invite = 5,
    approve = 6,
    kick = 7,
};

enum class NetworkResultCode : std::uint16_t {
    success = 0,
    pending_approval = 1,
    invalid_request = 2,
    not_found = 3,
    already_exists = 4,
    not_member = 5,
    permission_denied = 6,
    invite_required = 7,
    target_not_found = 8,
    limit_reached = 9,
    conflict = 10,
    internal_error = 11,
};

enum class NetworkEventKind : std::uint8_t {
    invited = 1,
    join_requested = 2,
    member_joined = 3,
    member_left = 4,
    member_kicked = 5,
};

struct NetworkCreateRequest {
    std::string name;

    bool operator==(const NetworkCreateRequest&) const = default;
};

struct NetworkListRequest {
    bool operator==(const NetworkListRequest&) const = default;
};

struct NetworkSelectionRequest {
    NetworkId network_id{};

    bool operator==(const NetworkSelectionRequest&) const = default;
};

struct NetworkMemberRequest {
    NetworkId network_id{};
    NetworkDeviceId device_id{};

    bool operator==(const NetworkMemberRequest&) const = default;
};

struct NetworkOperationResult {
    NetworkOperation operation = NetworkOperation::create;
    NetworkResultCode code = NetworkResultCode::success;
    NetworkId network_id{};

    bool operator==(const NetworkOperationResult&) const = default;
};

struct NetworkSummary {
    NetworkId network_id{};
    NetworkDeviceId owner_device_id{};
    NetworkRole role = NetworkRole::member;
    std::int64_t created_at_ms = 0;
    std::uint32_t member_count = 0;
    std::string name;

    bool operator==(const NetworkSummary&) const = default;
};

struct NetworkListResult {
    std::vector<NetworkSummary> networks;

    bool operator==(const NetworkListResult&) const = default;
};

struct NetworkEvent {
    NetworkEventKind kind = NetworkEventKind::invited;
    NetworkId network_id{};
    NetworkDeviceId device_id{};

    bool operator==(const NetworkEvent&) const = default;
};

[[nodiscard]] std::vector<std::byte> encode_network_create_request(
    const NetworkCreateRequest& request);
[[nodiscard]] NetworkCreateRequest decode_network_create_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_network_list_request(
    const NetworkListRequest& request = {});
[[nodiscard]] NetworkListRequest decode_network_list_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_network_selection_request(
    const NetworkSelectionRequest& request);
[[nodiscard]] NetworkSelectionRequest decode_network_selection_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_network_member_request(
    const NetworkMemberRequest& request);
[[nodiscard]] NetworkMemberRequest decode_network_member_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_network_operation_result(
    const NetworkOperationResult& result);
[[nodiscard]] NetworkOperationResult decode_network_operation_result(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_network_list_result(
    const NetworkListResult& result);
[[nodiscard]] NetworkListResult decode_network_list_result(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_network_event(const NetworkEvent& event);
[[nodiscard]] NetworkEvent decode_network_event(std::span<const std::byte> payload);

[[nodiscard]] bool is_known_network_operation(NetworkOperation operation) noexcept;
[[nodiscard]] bool is_known_network_result_code(NetworkResultCode code) noexcept;
[[nodiscard]] bool is_known_network_event_kind(NetworkEventKind kind) noexcept;
[[nodiscard]] std::string_view network_operation_name(NetworkOperation operation) noexcept;
[[nodiscard]] std::string_view network_result_code_name(NetworkResultCode code) noexcept;
[[nodiscard]] std::string_view network_event_kind_name(NetworkEventKind kind) noexcept;

}
