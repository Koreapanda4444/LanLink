#pragma once
#include "lanlink/protocol/network_messages.hpp"
namespace lanlink::protocol {
enum class AuditKind : std::uint8_t {
    network_create=1, network_join=2, network_leave=3, member_invite=4,
    member_approve=5, member_kick=6, password_change=7, invite_issue=8,
    invite_revoke=9, account_create=10, device_enroll=11, device_revoke=12,
    gateway_request=13, policy_change=14, enrollment_issue=15,
};
struct AuditEvent {
    std::uint64_t sequence=0, timestamp_ms=0;
    NetworkId network_id{};
    auth::DeviceId actor{}, target{};
    AuditKind kind=AuditKind::policy_change;
    NetworkResultCode result=NetworkResultCode::success;
    std::uint64_t policy_revision=0;
    bool operator==(const AuditEvent&) const = default;
};
[[nodiscard]] std::string_view audit_kind_name(AuditKind kind) noexcept;
[[nodiscard]] std::vector<std::byte> encode_audit_event(const AuditEvent& event);
[[nodiscard]] AuditEvent decode_audit_event(std::span<const std::byte> bytes);
}
