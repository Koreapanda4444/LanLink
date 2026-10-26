#pragma once

#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/network/virtual_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace lanlink::transport {

using WintunNetworkAddress = network::VirtualNetworkAddress;

class WintunAdapter final : public network::VirtualAdapter {
public:
    using PacketHandler = std::function<void(std::span<const std::byte>)>;

    explicit WintunAdapter(std::wstring name = L"LanLink",
                           std::uint32_t mtu = 1400,
                           std::uint32_t ring_capacity = 4U * 1024U * 1024U);
    ~WintunAdapter() override;

    WintunAdapter(const WintunAdapter&) = delete;
    WintunAdapter& operator=(const WintunAdapter&) = delete;
    WintunAdapter(WintunAdapter&&) = delete;
    WintunAdapter& operator=(WintunAdapter&&) = delete;

    void configure(const WintunNetworkAddress& address) override;
    void remove(const protocol::NetworkId& network_id) noexcept override;
    void start(PacketHandler handler) override;
    void inject(std::span<const std::byte> packet) override;
    void stop() noexcept override;
    [[nodiscard]] std::exception_ptr pump_error() const noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
