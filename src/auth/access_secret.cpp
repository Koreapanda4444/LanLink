#include "lanlink/auth/access_secret.hpp"
#include <openssl/evp.h>
#include <stdexcept>

namespace lanlink::auth {
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
