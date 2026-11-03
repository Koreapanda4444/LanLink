#include "lanlink/auth/relay_health.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
#include <stdexcept>

namespace lanlink::auth {
namespace {
void text(protocol::BinaryWriter& out,const std::string& value) {
    if(value.empty() || value.size()>64 || !std::all_of(value.begin(),value.end(),[](unsigned char c){
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='.';
    }))throw std::invalid_argument("invalid health identity");
    out.put<std::uint16_t>(static_cast<std::uint16_t>(value.size()));out.append(std::as_bytes(std::span(value.data(),value.size())));
}
std::string text(protocol::BinaryReader& in) {
    const auto value=in.bytes(in.get<std::uint16_t>());return {reinterpret_cast<const char*>(value.data()),value.size()};
}
std::vector<std::byte> body(const RelayHealth& value) {
    if(value.nonce==Nonce{} || !value.capacity || value.capacity>1'000'000 || value.active_connections>value.capacity)
        throw std::invalid_argument("invalid health counters or challenge");
    protocol::BinaryWriter out;out.put<std::uint32_t>(0x4c4c4831);out.put<std::uint16_t>(protocol::protocol_version);
    text(out,value.relay_id);text(out,value.authority);out.append(value.nonce);out.put<std::uint64_t>(value.issued_ms);
    out.put<std::uint64_t>(value.uptime_ms);out.put<std::uint32_t>(value.capacity);out.put<std::uint32_t>(value.active_connections);
    out.put<std::uint32_t>(value.upstream_rtt_us);
    out.put<std::uint64_t>(value.forwarded_packets);out.put<std::uint64_t>(value.forwarded_bytes);out.put<std::uint64_t>(value.dropped_packets);
    out.put<std::uint8_t>(value.ready?1:0);return std::move(out).finish();
}
}
std::vector<std::byte> sign_relay_health(const RelayHealth& value,const DeviceIdentity& identity) {
    auto result=body(value);const auto signature=identity.sign(result);result.insert(result.end(),signature.begin(),signature.end());return result;
}
RelayHealth verify_relay_health(std::span<const std::byte> encoded,const RelayEndpoint& expected,const Nonce& nonce,std::uint64_t now_ms) {
    if(encoded.size()<64 || encoded.size()>512 || expected.health_key==PublicKey{} || nonce==Nonce{})
        throw std::invalid_argument("invalid signed health response");
    const auto bytes=encoded.first(encoded.size()-signature_size);Signature signature{};std::copy(encoded.end()-signature_size,encoded.end(),signature.begin());
    if(!verify_signature(expected.health_key,bytes,signature))throw std::runtime_error("relay health signature rejected");
    protocol::BinaryReader in(bytes);
    if(in.get<std::uint32_t>()!=0x4c4c4831 || in.get<std::uint16_t>()!=expected.protocol_version)
        throw std::invalid_argument("relay health protocol mismatch");
    RelayHealth value;value.relay_id=text(in);value.authority=text(in);value.nonce=in.array<nonce_size>();value.issued_ms=in.get<std::uint64_t>();
    value.uptime_ms=in.get<std::uint64_t>();value.capacity=in.get<std::uint32_t>();value.active_connections=in.get<std::uint32_t>();
    value.upstream_rtt_us=in.get<std::uint32_t>();
    value.forwarded_packets=in.get<std::uint64_t>();value.forwarded_bytes=in.get<std::uint64_t>();value.dropped_packets=in.get<std::uint64_t>();
    const auto ready=in.get<std::uint8_t>();if(ready>1)throw std::invalid_argument("invalid health readiness");value.ready=ready!=0;in.finish();
    static_cast<void>(body(value));
    const auto age=value.issued_ms>now_ms?value.issued_ms-now_ms:now_ms-value.issued_ms;
    if(value.relay_id!=expected.id || value.authority!=expected.authority || value.capacity!=expected.capacity || value.nonce!=nonce || age>30'000)
        throw std::runtime_error("relay health challenge, freshness or authority mismatch");
    return value;
}
}
