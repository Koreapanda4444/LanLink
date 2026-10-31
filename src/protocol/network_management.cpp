#include "lanlink/protocol/network_management.hpp"
#include "lanlink/protocol/binary.hpp"
namespace lanlink::protocol {
namespace {
void validate(ManagementAction a,const NetworkId& id) {if(a<ManagementAction::inspect || a>ManagementAction::request_gateway || id==NetworkId{})throw std::invalid_argument("invalid management request");}
void policy(BinaryWriter& w,const NetworkPolicy& p){const auto bytes=encode_network_policy(p);w.put<std::uint32_t>(static_cast<std::uint32_t>(bytes.size()));w.append(bytes);}
NetworkPolicy policy(BinaryReader& r){return decode_network_policy(r.bytes(r.get<std::uint32_t>()));}
}
std::vector<std::byte> encode_management_request(const ManagementRequest& q){validate(q.action,q.network_id);BinaryWriter w;w.put<std::uint8_t>(1);w.put<std::uint8_t>(static_cast<std::uint8_t>(q.action));w.append(q.network_id);policy(w,q.policy);return std::move(w).finish();}
ManagementRequest decode_management_request(std::span<const std::byte> b){BinaryReader r(b);if(r.get<std::uint8_t>()!=1)throw std::invalid_argument("unknown management version");ManagementRequest q;q.action=static_cast<ManagementAction>(r.get<std::uint8_t>());q.network_id=r.array<16>();q.policy=policy(r);r.finish();static_cast<void>(encode_management_request(q));return q;}
std::vector<std::byte> encode_management_result(const ManagementResult& q){validate(q.action,q.network_id);if(!is_known_network_result_code(q.code))throw std::invalid_argument("invalid management result");BinaryWriter w;w.put<std::uint8_t>(1);w.put<std::uint8_t>(static_cast<std::uint8_t>(q.action));w.append(q.network_id);w.put<std::uint16_t>(static_cast<std::uint16_t>(q.code));policy(w,q.policy);return std::move(w).finish();}
ManagementResult decode_management_result(std::span<const std::byte> b){BinaryReader r(b);if(r.get<std::uint8_t>()!=1)throw std::invalid_argument("unknown management version");ManagementResult q;q.action=static_cast<ManagementAction>(r.get<std::uint8_t>());q.network_id=r.array<16>();q.code=static_cast<NetworkResultCode>(r.get<std::uint16_t>());q.policy=policy(r);r.finish();static_cast<void>(encode_management_result(q));return q;}
}
