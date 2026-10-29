#pragma once

#include "lanlink/protocol/network_messages.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace lanlink::network {

struct VirtualNetworkAddress {
    protocol::NetworkId network_id{};
    std::uint32_t address = 0;
    std::uint32_t subnet = 0;
    std::uint8_t prefix_length = 24;
    bool operator==(const VirtualNetworkAddress&) const = default;
};

class VirtualAdapter {
public:
    using PacketHandler = std::function<void(std::span<const std::byte>)>;
    virtual ~VirtualAdapter() = default;
    virtual void configure(const VirtualNetworkAddress& address) = 0;
    virtual void remove(const protocol::NetworkId& network_id) noexcept = 0;
    virtual void start(PacketHandler handler) = 0;
    virtual void inject(std::span<const std::byte> packet) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual std::exception_ptr pump_error() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::vector<std::uint32_t>> multicast_groups() const { return std::nullopt; }
    [[nodiscard]] virtual std::uint16_t packet_mtu() const noexcept { return 1400; }
};

}
