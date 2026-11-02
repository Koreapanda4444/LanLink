#pragma once
#include "lanlink/network/virtual_adapter.hpp"
#include <cstdint>
#include <memory>
namespace lanlink::platform {
[[nodiscard]] std::shared_ptr<network::VirtualAdapter> make_virtual_adapter(std::uint16_t mtu);
}
