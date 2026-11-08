#include "lanlink/protocol/device_registration.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
namespace lanlink::protocol {
std::vector<std::byte> registration_transcript(const DeviceRegistration& q) {
    if(q.code.size()!=40 || q.code.find_first_not_of("0123456789abcdef")!=q.code.npos || q.label.empty() || q.label.size()>128 || std::any_of(q.label.begin(),q.label.end(),[](unsigned char c){return c<32 || c==127;}))throw std::invalid_argument("Invalid device registration code or label");
    BinaryWriter w;constexpr std::string_view domain="LanLink device registration v1";w.append({reinterpret_cast<const std::byte*>(domain.data()),domain.size()});w.append(q.public_key);w.append(q.nonce);w.put<std::uint8_t>(static_cast<std::uint8_t>(q.code.size()));w.append({reinterpret_cast<const std::byte*>(q.code.data()),q.code.size()});w.put<std::uint8_t>(static_cast<std::uint8_t>(q.label.size()));w.append({reinterpret_cast<const std::byte*>(q.label.data()),q.label.size()});return std::move(w).finish();
}
std::vector<std::byte> encode_device_registration(const DeviceRegistration& q) {static_cast<void>(registration_transcript(q));BinaryWriter w;w.append(q.public_key);w.append(q.nonce);w.append(q.signature);w.append({reinterpret_cast<const std::byte*>(q.code.data()),q.code.size()});w.put<std::uint8_t>(static_cast<std::uint8_t>(q.label.size()));w.append({reinterpret_cast<const std::byte*>(q.label.data()),q.label.size()});return std::move(w).finish();}
DeviceRegistration decode_device_registration(std::span<const std::byte> b){BinaryReader r(b);DeviceRegistration q;q.public_key=r.array<32>();q.nonce=r.array<32>();q.signature=r.array<64>();const auto code=r.bytes(40);q.code={reinterpret_cast<const char*>(code.data()),code.size()};const auto label=r.bytes(r.get<std::uint8_t>());q.label={reinterpret_cast<const char*>(label.data()),label.size()};r.finish();static_cast<void>(registration_transcript(q));return q;}
}
