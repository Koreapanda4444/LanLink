#pragma once
#include "lanlink/network/virtual_adapter.hpp"
#include <memory>
#include <string>
namespace lanlink::network {
class PosixTunAdapter final : public VirtualAdapter {
public:
    explicit PosixTunAdapter(std::uint16_t mtu = 1400, std::string name = {});
    ~PosixTunAdapter() override;
    void configure(const VirtualNetworkAddress&) override;
    void remove(const protocol::NetworkId&) noexcept override;
    void configure_routing(const RoutingConfiguration&) override;
    void start(PacketHandler) override;
    void inject(std::span<const std::byte>) override;
    void stop() noexcept override;
    [[nodiscard]] std::exception_ptr pump_error() const noexcept override;
    [[nodiscard]] std::uint16_t packet_mtu() const noexcept override;
    [[nodiscard]] std::string name() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
