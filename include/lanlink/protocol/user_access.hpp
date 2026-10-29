#pragma once
#include "lanlink/protocol/network_messages.hpp"

namespace lanlink::protocol {
enum class AccessAction : std::uint8_t {
    account_create = 1, account_status = 2, enrollment_issue = 3,
    device_enroll = 4, device_revoke = 5,
};
struct AccountDevice {
    auth::DeviceId device_id{};
    std::string label;
    bool revoked = false;
    bool operator==(const AccountDevice&) const = default;
};
struct AccessRequest {
    AccessAction action = AccessAction::account_status;
    NetworkId network_id{};
    auth::DeviceId device_id{};
    std::string text;
    std::string secret;
    std::uint32_t lifetime_seconds = 300;
    std::uint16_t max_uses = 1;
    bool operator==(const AccessRequest&) const = default;
};
struct AccessResult {
    AccessAction action = AccessAction::account_status;
    NetworkResultCode code = NetworkResultCode::success;
    auth::DeviceId account_id{};
    NetworkId network_id{};
    std::string text;
    std::string secret;
    std::vector<AccountDevice> devices;
    bool operator==(const AccessResult&) const = default;
};
[[nodiscard]] std::vector<std::byte> encode_access_request(const AccessRequest& request);
[[nodiscard]] AccessRequest decode_access_request(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_access_result(const AccessResult& result);
[[nodiscard]] AccessResult decode_access_result(std::span<const std::byte> payload);
}
