#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanlink::auth {

inline constexpr std::size_t public_key_size = 32;
inline constexpr std::size_t secret_key_size = 32;
inline constexpr std::size_t signature_size = 64;
inline constexpr std::size_t nonce_size = 32;
inline constexpr std::size_t token_proof_size = 32;
inline constexpr std::size_t device_id_size = 32;

using PublicKey = std::array<std::byte, public_key_size>;
using SecretKey = std::array<std::byte, secret_key_size>;
using Signature = std::array<std::byte, signature_size>;
using Nonce = std::array<std::byte, nonce_size>;
using TokenProof = std::array<std::byte, token_proof_size>;
using DeviceId = std::array<std::byte, device_id_size>;

class DeviceIdentity {
public:
    static DeviceIdentity load_or_create(const std::filesystem::path& path);

    ~DeviceIdentity();
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;
    DeviceIdentity(DeviceIdentity&& other) noexcept;
    DeviceIdentity& operator=(DeviceIdentity&& other) noexcept;

    [[nodiscard]] const PublicKey& public_key() const noexcept;
    [[nodiscard]] Signature sign(std::span<const std::byte> message) const;
    [[nodiscard]] DeviceId device_id() const;
    [[nodiscard]] std::string device_id_hex() const;

private:
    DeviceIdentity(PublicKey public_key, const SecretKey& secret_key) noexcept;

    PublicKey public_key_{};
    SecretKey secret_key_{};
};

class AuthToken {
public:
    static AuthToken load(const std::filesystem::path& path);
    static AuthToken from_secret(std::string_view secret);

    ~AuthToken();
    AuthToken(const AuthToken&) = delete;
    AuthToken& operator=(const AuthToken&) = delete;
    AuthToken(AuthToken&& other) noexcept;
    AuthToken& operator=(AuthToken&& other) noexcept;

    [[nodiscard]] TokenProof proof(std::span<const std::byte> message) const;
    [[nodiscard]] bool verify(std::span<const std::byte> message,
                              const TokenProof& proof) const;

private:
    explicit AuthToken(std::array<std::byte, token_proof_size>&& key) noexcept;

    std::array<std::byte, token_proof_size> key_{};
};

[[nodiscard]] Nonce random_nonce();
[[nodiscard]] bool verify_signature(const PublicKey& public_key,
                                    std::span<const std::byte> message,
                                    const Signature& signature);
[[nodiscard]] DeviceId make_device_id(const PublicKey& public_key);
[[nodiscard]] std::vector<std::byte> make_auth_transcript(const PublicKey& public_key,
                                                          const Nonce& client_nonce,
                                                          const Nonce& server_nonce);
[[nodiscard]] std::string hex_encode(std::span<const std::byte> bytes);

}
