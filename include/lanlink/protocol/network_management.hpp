#pragma once
#include "lanlink/protocol/network_messages.hpp"
namespace lanlink::protocol {
enum class ManagementAction : std::uint8_t { inspect = 1, replace_policy = 2, request_gateway = 3 };
struct ManagementRequest {
    ManagementAction action = ManagementAction::inspect;
    NetworkId network_id{};
    NetworkPolicy policy;
    bool operator==(const ManagementRequest&) const = default;
};
struct ManagementResult {
    ManagementAction action = ManagementAction::inspect;
    NetworkId network_id{};
    NetworkResultCode code = NetworkResultCode::success;
    std::vector<auth::DeviceId> pending_requests;
    NetworkPolicy policy;
    bool operator==(const ManagementResult&) const = default;
};
[[nodiscard]] std::vector<std::byte> encode_management_request(const ManagementRequest& request);
[[nodiscard]] ManagementRequest decode_management_request(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_management_result(const ManagementResult& result);
[[nodiscard]] ManagementResult decode_management_result(std::span<const std::byte> bytes);
}
