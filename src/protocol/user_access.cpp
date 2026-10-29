#include "lanlink/protocol/user_access.hpp"
#include "lanlink/protocol/binary.hpp"
#include <stdexcept>

namespace lanlink::protocol {
namespace {
void action(AccessAction a) {
    if (a < AccessAction::account_create || a > AccessAction::network_create_protected)
        throw std::invalid_argument("unknown access action");
}
void string(BinaryWriter& w, const std::string& s, std::size_t limit) {
    if (s.size() > limit || s.find_first_of(std::string("\0\r\n", 3)) != std::string::npos)
        throw std::invalid_argument("invalid access text");
    w.put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
    w.append(std::as_bytes(std::span(s.data(), s.size())));
}
std::string string(BinaryReader& r, std::size_t limit) {
    auto b = r.bytes(r.get<std::uint16_t>());
    if (b.size() > limit) throw std::invalid_argument("access text too long");
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
}
std::vector<std::byte> encode_access_request(const AccessRequest& x) {
    action(x.action);
    BinaryWriter w;
    w.put<std::uint8_t>(1); w.put<std::uint8_t>(static_cast<std::uint8_t>(x.action));
    w.append(x.network_id); w.append(x.device_id);
    string(w, x.text, 128); string(w, x.secret, 256);
    w.put<std::uint32_t>(x.lifetime_seconds); w.put<std::uint16_t>(x.max_uses);
    if(x.action>=AccessAction::password_set) w.put<std::uint8_t>(x.require_approval ? 1 : 0);
    return std::move(w).finish();
}
AccessRequest decode_access_request(std::span<const std::byte> payload) {
    BinaryReader r(payload);
    if (r.get<std::uint8_t>() != 1) throw std::invalid_argument("unsupported access version");
    AccessRequest x; x.action = static_cast<AccessAction>(r.get<std::uint8_t>());
    x.network_id = r.array<16>(); x.device_id = r.array<32>();
    x.text = string(r, 128); x.secret = string(r, 256);
    x.lifetime_seconds = r.get<std::uint32_t>(); x.max_uses = r.get<std::uint16_t>();
    if(x.action>=AccessAction::password_set) {
        const auto flag=r.get<std::uint8_t>();
        if(flag>1) throw std::invalid_argument("invalid approval policy");
        x.require_approval=flag!=0;
    }
    r.finish(); static_cast<void>(encode_access_request(x)); return x;
}
std::vector<std::byte> encode_access_result(const AccessResult& x) {
    action(x.action);
    if (!is_known_network_result_code(x.code) || x.devices.size() > 64)
        throw std::invalid_argument("invalid access result");
    BinaryWriter w; w.put<std::uint8_t>(1);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(x.action));
    w.put<std::uint16_t>(static_cast<std::uint16_t>(x.code));
    w.append(x.account_id); w.append(x.network_id);
    string(w, x.text, 128); string(w, x.secret, 512);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(x.devices.size()));
    for (const auto& d : x.devices) {
        w.append(d.device_id); string(w, d.label, 128); w.put<std::uint8_t>(d.revoked ? 1 : 0);
    }
    return std::move(w).finish();
}
AccessResult decode_access_result(std::span<const std::byte> payload) {
    BinaryReader r(payload);
    if (r.get<std::uint8_t>() != 1) throw std::invalid_argument("unsupported access version");
    AccessResult x; x.action = static_cast<AccessAction>(r.get<std::uint8_t>());
    x.code = static_cast<NetworkResultCode>(r.get<std::uint16_t>());
    x.account_id = r.array<32>(); x.network_id = r.array<16>();
    x.text = string(r, 128); x.secret = string(r, 512);
    const auto count = r.get<std::uint8_t>();
    if (count > 64) throw std::invalid_argument("too many account devices");
    for (unsigned i = 0; i < count; ++i) {
        AccountDevice d; d.device_id = r.array<32>(); d.label = string(r, 128);
        const auto flag = r.get<std::uint8_t>();
        if (flag > 1) throw std::invalid_argument("invalid device state");
        d.revoked = flag != 0; x.devices.push_back(std::move(d));
    }
    r.finish(); static_cast<void>(encode_access_result(x)); return x;
}
}
