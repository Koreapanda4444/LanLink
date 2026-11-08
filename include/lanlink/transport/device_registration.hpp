#pragma once
#include "lanlink/protocol/device_registration.hpp"
#include <atomic>
#include "lanlink/transport/quic.hpp"
namespace lanlink::transport {
std::string register_device(const QuicClientOptions& options, const auth::DeviceIdentity& identity,
                            std::string_view code, std::string_view label, const std::atomic_bool& stopping);
}
