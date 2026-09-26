#pragma once

#include "lanlink/auth/device_keys.hpp"
#include "lanlink/protocol/network_messages.hpp"

namespace lanlink::auth {

using NetworkKey = Secret32;

[[nodiscard]] bool verify_network_key_envelope(
    const protocol::NetworkKeyEnvelope& envelope,
    const PublicKey& owner_signing_public_key);
[[nodiscard]] protocol::NetworkKeyEnvelope seal_network_key(
    const DeviceIdentity& owner_identity,
    const DeviceEncryptionKey& owner_encryption_key,
    const SignedDeviceKey& recipient_key,
    const DeviceId& recipient_id,
    const protocol::NetworkId& network_id,
    std::uint64_t epoch,
    const NetworkKey& network_key);
[[nodiscard]] NetworkKey open_network_key(
    const DeviceEncryptionKey& recipient_encryption_key,
    const protocol::NetworkKeyEnvelope& envelope,
    const SignedDeviceKey& owner_key,
    const DeviceId& recipient_id);

}
