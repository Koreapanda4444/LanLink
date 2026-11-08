#include "lanlink/relay/device_authority.hpp"
#include <openssl/crypto.h>
namespace lanlink::relay {
namespace {auth::DeviceId code_digest(std::string_view code){return auth::access_digest("LanLink server enrollment v1/"+std::string(code));}}
std::string DeviceAuthority::credential(const auth::DeviceId& id)const {
    constexpr std::string_view domain="LanLink individual relay credential v1/";
    std::vector<std::byte> input(reinterpret_cast<const std::byte*>(domain.data()),reinterpret_cast<const std::byte*>(domain.data()+domain.size()));input.insert(input.end(),id.begin(),id.end());return auth::hex_encode(master_.proof(input));
}
std::string DeviceAuthority::issue_code(std::int64_t now,std::uint32_t lifetime){if(!lifetime || lifetime>86400)throw std::invalid_argument("Registration lifetime must be 1 to 86400 seconds");const auto code=auth::random_access_code();store_.issue_server_registration(code_digest(code),now,now+static_cast<std::int64_t>(lifetime)*1000);return code;}
void DeviceAuthority::revoke_code(std::string_view code){store_.revoke_server_registration(code_digest(code));}
bool DeviceAuthority::allowed(const auth::DeviceId& id)const{if(store_.device_revoked(id))return false;const auto status=store_.server_device_revoked(id);return status ? !*status : !required_;}
bool DeviceAuthority::verify(const auth::PublicKey& key,std::span<const std::byte> transcript,const auth::TokenProof& proof)const {
    const auto id=auth::make_device_id(key);const auto status=store_.server_device_revoked(id);
    if(!allowed(id))return false;
    if(!status)return !required_ && master_.verify(transcript,proof);
    auto secret=credential(id);const bool valid=auth::AuthToken::from_secret(secret).verify(transcript,proof);OPENSSL_cleanse(secret.data(),secret.size());return valid;
}
std::vector<std::byte> DeviceAuthority::register_device(std::span<const std::byte> payload,std::int64_t now) {
    const auto q=protocol::decode_device_registration(payload);const auto transcript=protocol::registration_transcript(q);
    const auto id=auth::make_device_id(q.public_key);
    if(!auth::verify_signature(q.public_key,transcript,q.signature) || !store_.register_server_device(code_digest(q.code),id,q.label,now))return {std::byte{0}};
    auto secret=credential(id);std::vector<std::byte> result{std::byte{1}};result.insert(result.end(),reinterpret_cast<const std::byte*>(secret.data()),reinterpret_cast<const std::byte*>(secret.data()+secret.size()));OPENSSL_cleanse(secret.data(),secret.size());return result;
}
}
