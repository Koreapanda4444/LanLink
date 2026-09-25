#pragma once

#include "lanlink/auth/identity.hpp"

#include <memory>

namespace lanlink::auth {

struct SignedDeviceKey {
    PublicKey identity_public_key{};
    EncryptionPublicKey encryption_public_key{};
    Nonce client_nonce{};
    Nonce server_nonce{};
    Signature signature{};

    bool operator==(const SignedDeviceKey&) const = default;
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

private:
    struct Impl;
    explicit DeviceEncryptionKey(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool verify_signed_device_key(const SignedDeviceKey& key,
                                            const DeviceId& expected_device_id);

}
