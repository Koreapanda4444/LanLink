#pragma once

#include "lanlink/auth/identity.hpp"

#include <memory>
#include <span>

namespace lanlink::auth {

struct SignedDeviceKey {
    PublicKey identity_public_key{};
    EncryptionPublicKey encryption_public_key{};
    Nonce client_nonce{};
    Nonce server_nonce{};
    Signature signature{};

    bool operator==(const SignedDeviceKey&) const = default;
};

class Secret32 {
public:
    static Secret32 random();

    explicit Secret32(std::array<std::byte, 32> bytes) noexcept;
    ~Secret32();
    Secret32(const Secret32& other) noexcept;
    Secret32& operator=(const Secret32& other) noexcept;
    Secret32(Secret32&& other) noexcept;
    Secret32& operator=(Secret32&& other) noexcept;

    [[nodiscard]] std::span<const std::byte, 32> bytes() const noexcept;
    [[nodiscard]] bool operator==(const Secret32& other) const noexcept;

private:
    std::array<std::byte, 32> bytes_{};
};

class DeviceEncryptionKey {
public:
    static DeviceEncryptionKey generate();

    ~DeviceEncryptionKey();
    DeviceEncryptionKey(const DeviceEncryptionKey&) = delete;
    DeviceEncryptionKey& operator=(const DeviceEncryptionKey&) = delete;
    DeviceEncryptionKey(DeviceEncryptionKey&&) noexcept;
    DeviceEncryptionKey& operator=(DeviceEncryptionKey&&) noexcept;

    [[nodiscard]] const EncryptionPublicKey& public_key() const noexcept;
    [[nodiscard]] Secret32 derive_shared_secret(
        const EncryptionPublicKey& peer_public_key) const;

private:
    struct Impl;
    explicit DeviceEncryptionKey(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool verify_signed_device_key(const SignedDeviceKey& key,
                                            const DeviceId& expected_device_id);

}
