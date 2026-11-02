#pragma once
#include "lanlink/network/virtual_adapter.hpp"
#include <memory>
#include <string>
namespace lanlink::platform {
class TunDevice {
public:
    virtual ~TunDevice() = default;
    [[nodiscard]] virtual int descriptor() const noexcept = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::size_t prefix_size() const noexcept { return 0; }
    virtual void configure(const network::VirtualNetworkAddress&) = 0;
    virtual void remove(const protocol::NetworkId&) noexcept = 0;
    virtual void routing(const network::RoutingConfiguration&) = 0;
};
[[nodiscard]] std::unique_ptr<TunDevice> open_tun(std::uint16_t mtu, const std::string& name);
}
