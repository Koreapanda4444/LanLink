#include "lanlink/auth/network_keys.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace lanlink::auth {
namespace {

using DerivationContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

constexpr std::string_view wrap_domain = "lanlink-network-key-wrap-v1";
constexpr std::string_view signature_domain = "lanlink-network-key-envelope-v1";
constexpr std::size_t authenticated_header_size =
    protocol::network_id_size + 8 + 2 * protocol::network_device_id_size +
    2 * encryption_public_key_size + protocol::network_key_nonce_size;

std::vector<std::byte> authenticated_header(
    const protocol::NetworkKeyEnvelope& envelope) {
    auto bytes = protocol::encode_network_key_envelope(envelope);
    bytes.resize(authenticated_header_size);
    return bytes;
}

std::vector<std::byte> signed_transcript(
    const protocol::NetworkKeyEnvelope& envelope) {
    auto wire = protocol::encode_network_key_envelope(envelope);
    wire.resize(wire.size() - signature_size);
    std::vector<std::byte> transcript;
    transcript.reserve(signature_domain.size() + wire.size());
    transcript.insert(transcript.end(),
                      reinterpret_cast<const std::byte*>(signature_domain.data()),
                      reinterpret_cast<const std::byte*>(signature_domain.data() +
                                                         signature_domain.size()));
    transcript.insert(transcript.end(), wire.begin(), wire.end());
    return transcript;
}

Secret32 derive_wrapping_key(const Secret32& shared,
                             const std::span<const std::byte> header) {
    DerivationContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr),
                              EVP_PKEY_CTX_free);
    std::array<std::byte, 32> output{};
    auto size = output.size();
    if (!context || EVP_PKEY_derive_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(
            context.get(), reinterpret_cast<const unsigned char*>(header.data()),
            static_cast<int>(header.size())) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(
            context.get(), reinterpret_cast<const unsigned char*>(shared.bytes().data()),
            static_cast<int>(shared.bytes().size())) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            context.get(), reinterpret_cast<const unsigned char*>(wrap_domain.data()),
            static_cast<int>(wrap_domain.size())) != 1 ||
        EVP_PKEY_derive(context.get(),
                        reinterpret_cast<unsigned char*>(output.data()), &size) != 1 ||
        size != output.size()) {
        OPENSSL_cleanse(output.data(), output.size());
        throw std::runtime_error("network wrapping key derivation failed");
    }
    Secret32 result(output);
    OPENSSL_cleanse(output.data(), output.size());
    return result;
}

void encrypt_key(const NetworkKey& key,
                 const Secret32& wrapping_key,
                 const std::span<const std::byte> header,
                 protocol::NetworkKeyEnvelope& envelope) {
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int used = 0;
    int output = 0;
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr,
                                       nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(envelope.nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(
            context.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(wrapping_key.bytes().data()),
            reinterpret_cast<const unsigned char*>(envelope.nonce.data())) != 1 ||
        EVP_EncryptUpdate(context.get(), nullptr, &output,
                          reinterpret_cast<const unsigned char*>(header.data()),
                          static_cast<int>(header.size())) != 1 ||
        EVP_EncryptUpdate(
            context.get(), reinterpret_cast<unsigned char*>(envelope.ciphertext.data()),
            &used, reinterpret_cast<const unsigned char*>(key.bytes().data()),
            static_cast<int>(key.bytes().size())) != 1 ||
        used != static_cast<int>(envelope.ciphertext.size()) ||
        EVP_EncryptFinal_ex(
            context.get(),
            reinterpret_cast<unsigned char*>(envelope.ciphertext.data()) + used,
            &output) != 1 || output != 0 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(envelope.tag.size()),
                            envelope.tag.data()) != 1) {
        throw std::runtime_error("network key encryption failed");
    }
}

NetworkKey decrypt_key(const protocol::NetworkKeyEnvelope& envelope,
                       const Secret32& wrapping_key,
                       const std::span<const std::byte> header) {
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    std::array<std::byte, 32> plaintext{};
    int used = 0;
    int output = 0;
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr,
                                       nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(envelope.nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(
            context.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(wrapping_key.bytes().data()),
            reinterpret_cast<const unsigned char*>(envelope.nonce.data())) != 1 ||
        EVP_DecryptUpdate(context.get(), nullptr, &output,
                          reinterpret_cast<const unsigned char*>(header.data()),
                          static_cast<int>(header.size())) != 1 ||
        EVP_DecryptUpdate(
            context.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &used,
            reinterpret_cast<const unsigned char*>(envelope.ciphertext.data()),
            static_cast<int>(envelope.ciphertext.size())) != 1 ||
        used != static_cast<int>(plaintext.size()) ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(envelope.tag.size()),
                            const_cast<std::byte*>(envelope.tag.data())) != 1 ||
        EVP_DecryptFinal_ex(context.get(),
                            reinterpret_cast<unsigned char*>(plaintext.data()) + used,
                            &output) != 1 || output != 0) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        throw std::runtime_error("network key authentication failed");
    }
    NetworkKey result(plaintext);
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    return result;
}

}

bool verify_network_key_envelope(
    const protocol::NetworkKeyEnvelope& envelope,
    const PublicKey& owner_signing_public_key) {
    if (make_device_id(owner_signing_public_key) != envelope.owner_device_id) {
        return false;
    }
    try {
        return verify_signature(owner_signing_public_key,
                                signed_transcript(envelope), envelope.signature);
    } catch (const std::invalid_argument&) {
        return false;
    }
}

protocol::NetworkKeyEnvelope seal_network_key(
    const DeviceIdentity& owner_identity,
    const DeviceEncryptionKey& owner_encryption_key,
    const SignedDeviceKey& recipient_key,
    const DeviceId& recipient_id,
    const protocol::NetworkId& network_id,
    const std::uint64_t epoch,
    const NetworkKey& network_key) {
    if (!verify_signed_device_key(recipient_key, recipient_id) || epoch == 0 ||
        owner_identity.device_id() == recipient_id) {
        throw std::invalid_argument("network key recipient is invalid");
    }

    protocol::NetworkKeyEnvelope envelope;
    envelope.network_id = network_id;
    envelope.epoch = epoch;
    envelope.owner_device_id = owner_identity.device_id();
    envelope.recipient_device_id = recipient_id;
    envelope.owner_encryption_public_key = owner_encryption_key.public_key();
    envelope.recipient_encryption_public_key = recipient_key.encryption_public_key;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(envelope.nonce.data()),
                   static_cast<int>(envelope.nonce.size())) != 1) {
        throw std::runtime_error("network key nonce generation failed");
    }

    const auto header = authenticated_header(envelope);
    const auto shared = owner_encryption_key.derive_shared_secret(
        envelope.recipient_encryption_public_key);
    const auto wrapping_key = derive_wrapping_key(shared, header);
    encrypt_key(network_key, wrapping_key, header, envelope);
    envelope.signature = owner_identity.sign(signed_transcript(envelope));
    return envelope;
}

NetworkKey open_network_key(
    const DeviceEncryptionKey& recipient_encryption_key,
    const protocol::NetworkKeyEnvelope& envelope,
    const SignedDeviceKey& owner_key,
    const DeviceId& recipient_id) {
    if (envelope.recipient_device_id != recipient_id ||
        envelope.recipient_encryption_public_key != recipient_encryption_key.public_key() ||
        !verify_signed_device_key(owner_key, envelope.owner_device_id) ||
        owner_key.encryption_public_key != envelope.owner_encryption_public_key ||
        !verify_network_key_envelope(envelope, owner_key.identity_public_key)) {
        throw std::runtime_error("network key sender or recipient is invalid");
    }

    const auto header = authenticated_header(envelope);
    const auto shared = recipient_encryption_key.derive_shared_secret(
        envelope.owner_encryption_public_key);
    const auto wrapping_key = derive_wrapping_key(shared, header);
    return decrypt_key(envelope, wrapping_key, header);
}

}
