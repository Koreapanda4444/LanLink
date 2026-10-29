#include "lanlink/auth/directory.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace lanlink::auth {
namespace {
void text(protocol::BinaryWriter& w, const std::string& s, std::size_t limit) {
    if (s.empty() || s.size()>limit || !std::all_of(s.begin(),s.end(),[](unsigned char c){
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-';
    })) throw std::invalid_argument("invalid directory endpoint");
    w.put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
    w.append(std::as_bytes(std::span(s.data(),s.size())));
}
std::string text(protocol::BinaryReader& r) {
    const auto b=r.bytes(r.get<std::uint16_t>());
    return std::string(reinterpret_cast<const char*>(b.data()),b.size());
}
std::vector<std::byte> body(const DirectoryCatalog& c) {
    if (c.relays.empty() || c.relays.size()>32 || c.expires_ms<=c.issued_ms || c.expires_ms-c.issued_ms>86400000)
        throw std::invalid_argument("invalid directory lifetime or size");
    protocol::BinaryWriter w; w.put<std::uint32_t>(0x4c4c4431);
    w.put<std::uint64_t>(c.issued_ms); w.put<std::uint64_t>(c.expires_ms);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(c.relays.size()));
    std::set<std::string> ids;
    for (const auto& e:c.relays) {
        if (!ids.insert(e.id).second || !e.port) throw std::invalid_argument("duplicate directory ID or invalid port");
        text(w,e.id,64); text(w,e.host,253); w.put<std::uint16_t>(e.port);
    }
    return std::move(w).finish();
}
}
PublicKey parse_directory_key(std::string_view hex) {
    if (hex.size()!=64) throw std::invalid_argument("directory key requires 64 hex digits");
    auto digit=[](char c)->unsigned {
        if (c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        if(c>='A' && c<='F') return c-'A'+10;
        throw std::invalid_argument("invalid directory key");
    };
    PublicKey key{}; for(std::size_t i=0;i<32;++i) key[i]=static_cast<std::byte>((digit(hex[i*2])<<4)|digit(hex[i*2+1]));
    if (std::all_of(key.begin(),key.end(),[](auto b){return b==std::byte{0};})) throw std::invalid_argument("zero directory key");
    return key;
}
std::vector<std::byte> sign_directory(const DirectoryCatalog& c, const DeviceIdentity& identity) {
    auto bytes=body(c); const auto signature=identity.sign(bytes);
    bytes.insert(bytes.end(),signature.begin(),signature.end()); return bytes;
}
DirectoryCatalog verify_directory(std::span<const std::byte> response, const PublicKey& key, std::uint64_t now_ms) {
    if (response.size()<85 || response.size()>16384) throw std::invalid_argument("invalid directory response size");
    Signature signature{}; std::copy(response.end()-64,response.end(),signature.begin());
    const auto payload=response.first(response.size()-64);
    if (!verify_signature(key,payload,signature)) throw std::invalid_argument("directory signature rejected");
    protocol::BinaryReader r(payload);
    if (r.get<std::uint32_t>()!=0x4c4c4431) throw std::invalid_argument("invalid directory version");
    DirectoryCatalog c; c.issued_ms=r.get<std::uint64_t>(); c.expires_ms=r.get<std::uint64_t>();
    const auto count=r.get<std::uint8_t>();
    if(count>32) throw std::invalid_argument("directory limit exceeded");
    for(unsigned i=0;i<count;++i) { auto id=text(r); auto host=text(r); const auto port=r.get<std::uint16_t>(); c.relays.push_back({id,host,port}); }
    r.finish(); static_cast<void>(body(c));
    if(now_ms<c.issued_ms || now_ms>=c.expires_ms) throw std::invalid_argument("directory response expired or not yet valid");
    return c;
}
BootstrapResult resolve_bootstrap(const core::RuntimeConfig& config, const DirectoryFetcher& fetch, std::uint64_t now_ms) {
    BootstrapResult result; result.relay={"manual",config.relay_host,config.relay_port};
    if(config.bootstrap_mode=="manual") return result;
    try {
        const auto c=verify_directory(fetch(config.directory_url),parse_directory_key(config.directory_public_key),now_ms);
        const auto e=std::find_if(c.relays.begin(),c.relays.end(),[&](const auto& x){return config.directory_relay_id.empty() || x.id==config.directory_relay_id;});
        if(e==c.relays.end()) throw std::runtime_error("configured relay is absent from directory");
        result.relay=*e; result.from_directory=true;
    } catch(const std::exception&) { result.error="Directory unavailable or untrusted. Using the configured manual relay."; }
    return result;
}
}
