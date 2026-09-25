#include "lanlink/auth/device_keys.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace lanlink::auth {
namespace {

using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](const std::byte byte) {
        return byte == std::byte{0};
    });
}

}

struct DeviceEncryptionKey::Impl {
    Impl(Pkey key_value, EncryptionPublicKey public_key_value)
        : key(std::move(key_value)), public_key(std::move(public_key_value)) {
    }

    Pkey key;
    EncryptionPublicKey public_key;
};

DeviceEncryptionKey::DeviceEncryptionKey(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

DeviceEncryptionKey::~DeviceEncryptionKey() = default;
DeviceEncryptionKey::DeviceEncryptionKey(DeviceEncryptionKey&&) noexcept = default;
DeviceEncryptionKey& DeviceEncryptionKey::operator=(DeviceEncryptionKey&&) noexcept = default;

DeviceEncryptionKey DeviceEncryptionKey::generate() {
    PkeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1) {
        throw std::runtime_error("X25519 key generation failed");
    }

    EVP_PKEY* raw_key = nullptr;
    const auto status = EVP_PKEY_keygen(context.get(), &raw_key);
    Pkey key(raw_key, EVP_PKEY_free);
    if (status != 1 || !key) {
        throw std::runtime_error("X25519 key generation failed");
    }
    EncryptionPublicKey public_key{};
    auto size = public_key.size();
    if (EVP_PKEY_get_raw_public_key(key.get(),
                                    reinterpret_cast<unsigned char*>(public_key.data()),
                                    &size) != 1 ||
        size != public_key.size()) {
        throw std::runtime_error("X25519 public key extraction failed");
    }

    return DeviceEncryptionKey(std::make_unique<Impl>(std::move(key), public_key));
}

const EncryptionPublicKey& DeviceEncryptionKey::public_key() const noexcept {
    return impl_->public_key;
}

bool verify_signed_device_key(const SignedDeviceKey& key,
                              const DeviceId& expected_device_id) {
    if (all_zero(expected_device_id) || all_zero(key.encryption_public_key) ||
        make_device_id(key.identity_public_key) != expected_device_id) {
        return false;
    }

    const auto transcript = make_auth_transcript(key.identity_public_key,
                                                 key.encryption_public_key,
                                                 key.client_nonce,
                                                 key.server_nonce);
    return verify_signature(key.identity_public_key, transcript, key.signature);
}

}
