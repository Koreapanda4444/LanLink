#include "lanlink/protocol/audit_event.hpp"
#include "lanlink/protocol/binary.hpp"
#include <limits>
namespace lanlink::protocol {
std::string_view audit_kind_name(AuditKind k) noexcept {
    switch(k){
    case AuditKind::network_create:return "Network created";
    case AuditKind::network_join:return "Member joined or requested access";
    case AuditKind::network_leave:return "Member left";
    case AuditKind::member_invite:return "Member invited";
    case AuditKind::member_approve:return "Member approved";
    case AuditKind::member_kick:return "Member removed";
    case AuditKind::password_change:return "Network password changed";
    case AuditKind::invite_issue:return "Invitation link issued";
    case AuditKind::invite_revoke:return "Invitation links revoked";
    case AuditKind::account_create:return "Account created";
    case AuditKind::device_enroll:return "Device enrolled";
    case AuditKind::device_revoke:return "Device revoked";
    case AuditKind::gateway_request:return "Gateway requested";
    case AuditKind::policy_change:return "Network policy changed";
    case AuditKind::enrollment_issue:return "Device enrollment code issued";
    }return "Unknown event";
}
std::vector<std::byte> encode_audit_event(const AuditEvent& e) {
    if(e.actor==auth::DeviceId{} || e.kind<AuditKind::network_create || e.kind>AuditKind::enrollment_issue || !is_known_network_result_code(e.result) ||
        e.timestamp_ms>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || e.policy_revision>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))throw std::invalid_argument("invalid audit event");
    BinaryWriter w;w.put<std::uint64_t>(e.sequence);w.put<std::uint64_t>(e.timestamp_ms);w.append(e.network_id);w.append(e.actor);w.append(e.target);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(e.kind));w.put<std::uint16_t>(static_cast<std::uint16_t>(e.result));w.put<std::uint64_t>(e.policy_revision);return std::move(w).finish();
}
AuditEvent decode_audit_event(std::span<const std::byte> b){BinaryReader r(b);AuditEvent e;e.sequence=r.get<std::uint64_t>();e.timestamp_ms=r.get<std::uint64_t>();e.network_id=r.array<16>();e.actor=r.array<32>();e.target=r.array<32>();e.kind=static_cast<AuditKind>(r.get<std::uint8_t>());e.result=static_cast<NetworkResultCode>(r.get<std::uint16_t>());e.policy_revision=r.get<std::uint64_t>();r.finish();static_cast<void>(encode_audit_event(e));return e;}
}
