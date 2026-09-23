#include "lanlink/auth/identity.hpp"
#include "secret_store.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace lanlink::auth {
namespace {

constexpr std::size_t maximum_token_size = 4096;
constexpr std::string_view transcript_prefix = "lanlink-auth-v1";

using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

void cleanse(const std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
}

Pkey private_key(const SecretKey& secret_key) {
    auto* raw = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519,
        nullptr,
        reinterpret_cast<const unsigned char*>(secret_key.data()),
        secret_key.size());

    if (raw == nullptr) {
        throw std::runtime_error("cannot create Ed25519 private key");
    }

    return Pkey(raw, EVP_PKEY_free);
}

Pkey public_key(const PublicKey& public_key_value) {
    auto* raw = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519,
        nullptr,
        reinterpret_cast<const unsigned char*>(public_key_value.data()),
        public_key_value.size());

    if (raw == nullptr) {
        throw std::runtime_error("cannot create Ed25519 public key");
    }

    return Pkey(raw, EVP_PKEY_free);
}

PublicKey derive_public_key(const SecretKey& secret_key) {
    const auto key = private_key(secret_key);
    PublicKey result{};
    auto length = result.size();

    if (EVP_PKEY_get_raw_public_key(
            key.get(), reinterpret_cast<unsigned char*>(result.data()), &length) != 1 ||
        length != result.size()) {
        throw std::runtime_error("cannot derive Ed25519 public key");
    }

    return result;
}

std::array<std::byte, 32> sha256(const std::span<const std::byte> input) {
    std::array<std::byte, 32> output{};
    unsigned int output_length = 0;
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);

    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(),
                           reinterpret_cast<unsigned char*>(output.data()),
                           &output_length) != 1 ||
        output_length != output.size()) {
        throw std::runtime_error("SHA-256 failed");
    }

    return output;
}

void random_fill(const std::span<std::byte> output) {
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        RAND_bytes(reinterpret_cast<unsigned char*>(output.data()),
                   static_cast<int>(output.size())) != 1) {
        throw std::runtime_error("secure random generation failed");
    }
}

std::vector<std::byte> read_binary_file(const std::filesystem::path& path,
                                        const std::size_t maximum_size) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);

    if (error) {
        throw std::runtime_error("cannot inspect secret file: " + path.string());
    }

    if (size > maximum_size) {
        throw std::runtime_error("secret file is too large: " + path.string());
    }

    std::ifstream input(path, std::ios::binary);

    if (!input) {
        throw std::runtime_error("cannot open secret file: " + path.string());
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    if (!input && !input.eof()) {
        throw std::runtime_error("cannot read secret file: " + path.string());
    }

    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        throw std::runtime_error("secret file changed while reading: " + path.string());
    }

    return bytes;
}

}

DeviceIdentity::DeviceIdentity(PublicKey public_key_value,
                               const SecretKey& secret_key) noexcept
    : public_key_(std::move(public_key_value)), secret_key_(secret_key) {
}

DeviceIdentity::~DeviceIdentity() {
    cleanse(secret_key_);
}

DeviceIdentity::DeviceIdentity(DeviceIdentity&& other) noexcept
    : public_key_(other.public_key_), secret_key_(other.secret_key_) {
    cleanse(other.secret_key_);
}

DeviceIdentity& DeviceIdentity::operator=(DeviceIdentity&& other) noexcept {
    if (this != &other) {
        cleanse(secret_key_);
        public_key_ = other.public_key_;
        secret_key_ = other.secret_key_;
        cleanse(other.secret_key_);
    }

    return *this;
}

DeviceIdentity DeviceIdentity::load_or_create(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("device identity path is empty");
    }

    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);

    if (error) {
        throw std::runtime_error("cannot inspect device identity: " + path.string());
    }

    SecretKey secret_key{};

    try {
        if (exists) {
            const auto requires_migration = detail::load_device_secret(path, secret_key);

            if (requires_migration) {
                detail::store_device_secret(path, secret_key);
            }
        } else {
            random_fill(secret_key);
            detail::store_device_secret(path, secret_key);
        }

        auto public_key_value = derive_public_key(secret_key);
        auto identity = DeviceIdentity(std::move(public_key_value), secret_key);
        cleanse(secret_key);
        return identity;
    } catch (...) {
        cleanse(secret_key);
        throw;
    }
}

const PublicKey& DeviceIdentity::public_key() const noexcept {
    return public_key_;
}

Signature DeviceIdentity::sign(const std::span<const std::byte> message) const {
    const auto key = private_key(secret_key_);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    Signature signature{};
    auto signature_length = signature.size();

    if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(context.get(),
                       reinterpret_cast<unsigned char*>(signature.data()),
                       &signature_length,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1 ||
        signature_length != signature.size()) {
        throw std::runtime_error("device signature failed");
    }

    return signature;
}

DeviceId DeviceIdentity::device_id() const {
    return make_device_id(public_key_);
}

std::string DeviceIdentity::device_id_hex() const {
    const auto id = device_id();
    return hex_encode(id);
}

AuthToken::AuthToken(std::array<std::byte, token_proof_size>&& key) noexcept : key_(key) {
    cleanse(key);
}

AuthToken::~AuthToken() {
    cleanse(key_);
}

AuthToken::AuthToken(AuthToken&& other) noexcept : key_(other.key_) {
    cleanse(other.key_);
}

AuthToken& AuthToken::operator=(AuthToken&& other) noexcept {
    if (this != &other) {
        cleanse(key_);
        key_ = other.key_;
        cleanse(other.key_);
    }

    return *this;
}

AuthToken AuthToken::load(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::invalid_argument("authentication token path is empty");
    }

    auto bytes = read_binary_file(path, maximum_token_size);

    while (!bytes.empty() && (bytes.back() == std::byte{'\n'} || bytes.back() == std::byte{'\r'})) {
        bytes.pop_back();
    }

    if (bytes.size() < 32) {
        cleanse(bytes);
        throw std::runtime_error("authentication token must contain at least 32 bytes");
    }

    if (std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end() ||
        std::find(bytes.begin(), bytes.end(), std::byte{'\n'}) != bytes.end() ||
        std::find(bytes.begin(), bytes.end(), std::byte{'\r'}) != bytes.end()) {
        cleanse(bytes);
        throw std::runtime_error("authentication token contains invalid characters");
    }

    try {
        auto key = sha256(bytes);
        cleanse(bytes);
        return AuthToken(std::move(key));
    } catch (...) {
        cleanse(bytes);
        throw;
    }
}

AuthToken AuthToken::from_secret(const std::string_view secret) {
    if (secret.size() < 32 || secret.size() > maximum_token_size) {
        throw std::invalid_argument("authentication token must contain 32 to 4096 bytes");
    }

    auto key = sha256(std::as_bytes(std::span<const char>(secret.data(), secret.size())));
    return AuthToken(std::move(key));
}

TokenProof AuthToken::proof(const std::span<const std::byte> message) const {
    TokenProof result{};
    std::size_t result_length = 0;
    const auto* output = EVP_Q_mac(nullptr,
                                   "HMAC",
                                   nullptr,
                                   "SHA256",
                                   nullptr,
                                   key_.data(),
                                   key_.size(),
                                   reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size(),
                                   reinterpret_cast<unsigned char*>(result.data()),
                                   result.size(),
                                   &result_length);

    if (output == nullptr || result_length != result.size()) {
        throw std::runtime_error("authentication token proof failed");
    }

    return result;
}

bool AuthToken::verify(const std::span<const std::byte> message,
                       const TokenProof& proof_value) const {
    auto expected = proof(message);
    const auto result = CRYPTO_memcmp(expected.data(), proof_value.data(), expected.size()) == 0;
    cleanse(expected);
    return result;
}

Nonce random_nonce() {
    Nonce nonce{};
    random_fill(nonce);
    return nonce;
}

SessionId random_session_id() {
    SessionId session_id{};
    random_fill(session_id);
    return session_id;
}

bool verify_signature(const PublicKey& public_key_value,
                      const std::span<const std::byte> message,
                      const Signature& signature) {
    const auto key = public_key(public_key_value);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);

    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        throw std::runtime_error("signature verification setup failed");
    }

    return EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signature.data()),
                            signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1;
}

DeviceId make_device_id(const PublicKey& public_key_value) {
    return sha256(public_key_value);
}

std::vector<std::byte> make_auth_transcript(const PublicKey& public_key_value,
                                            const Nonce& client_nonce,
                                            const Nonce& server_nonce) {
    std::vector<std::byte> transcript;
    transcript.reserve(transcript_prefix.size() + public_key_value.size() + client_nonce.size() +
                       server_nonce.size());
    transcript.insert(transcript.end(),
                      reinterpret_cast<const std::byte*>(transcript_prefix.data()),
                      reinterpret_cast<const std::byte*>(transcript_prefix.data() +
                                                         transcript_prefix.size()));
    transcript.insert(transcript.end(), public_key_value.begin(), public_key_value.end());
    transcript.insert(transcript.end(), client_nonce.begin(), client_nonce.end());
    transcript.insert(transcript.end(), server_nonce.begin(), server_nonce.end());
    return transcript;
}

std::string hex_encode(const std::span<const std::byte> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);

    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }

    return output;
}

}
