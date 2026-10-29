#pragma once

#include "lanlink/network/virtual_adapter.hpp"
#include <memory>
#include <string>

namespace lanlink::network {

class LinuxTapAdapter final : public VirtualAdapter {
public:
    explicit LinuxTapAdapter(std::string name = "lltap%d");
    ~LinuxTapAdapter() override;
    LinuxTapAdapter(const LinuxTapAdapter&) = delete;
    LinuxTapAdapter& operator=(const LinuxTapAdapter&) = delete;
    void configure(const VirtualNetworkAddress& address) override;
    void remove(const protocol::NetworkId& network_id) noexcept override;
    void start(PacketHandler handler) override;
    void inject(std::span<const std::byte> packet) override;
    void stop() noexcept override;
    [[nodiscard]] std::exception_ptr pump_error() const noexcept override;
    [[nodiscard]] std::uint16_t packet_mtu() const noexcept override { return 1400; }
    [[nodiscard]] std::string name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
