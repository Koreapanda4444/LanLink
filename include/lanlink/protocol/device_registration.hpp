#pragma once
#include "lanlink/auth/identity.hpp"
namespace lanlink::protocol {
struct DeviceRegistration {
    auth::PublicKey public_key{};
    auth::Nonce nonce{};
    auth::Signature signature{};
    std::string code, label;
};
std::vector<std::byte> registration_transcript(const DeviceRegistration& request);
std::vector<std::byte> encode_device_registration(const DeviceRegistration& request);
DeviceRegistration decode_device_registration(std::span<const std::byte> payload);
}
