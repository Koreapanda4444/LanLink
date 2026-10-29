#include "lanlink/auth/access_secret.hpp"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <stdexcept>

namespace lanlink::auth {
namespace {
DeviceId derive(std::string_view password,const Nonce& salt) {
    if(password.size()<8 || password.size()>128) throw std::invalid_argument("password must contain 8 to 128 bytes");
    DeviceId key{};
    if(EVP_PBE_scrypt(password.data(),password.size(),reinterpret_cast<const unsigned char*>(salt.data()),salt.size(),
        32768,8,1,64U*1024U*1024U,reinterpret_cast<unsigned char*>(key.data()),key.size())!=1)
        throw std::runtime_error("password derivation failed");
    return key;
}
}
PasswordVerifier protect_password(std::string_view password,bool require_approval) {
    PasswordVerifier verifier; verifier.salt=random_nonce(); verifier.digest=derive(password,verifier.salt);
    verifier.require_approval=require_approval; return verifier;
}
bool password_matches(std::string_view password,const PasswordVerifier& verifier) {
    auto derived=derive(password,verifier.salt);
    const bool match=CRYPTO_memcmp(derived.data(),verifier.digest.data(),derived.size())==0;
    OPENSSL_cleanse(derived.data(),derived.size()); return match;
}
DeviceId access_digest(std::string_view secret) {
    DeviceId digest{}; unsigned length = 0;
    if (EVP_Digest(secret.data(), secret.size(), reinterpret_cast<unsigned char*>(digest.data()),
                   &length, EVP_sha256(), nullptr) != 1 || length != digest.size())
        throw std::runtime_error("access digest failed");
    return digest;
}
std::string random_access_code() {
    const auto nonce = random_nonce();
    return hex_encode(std::span(nonce).first(20));
}
}
