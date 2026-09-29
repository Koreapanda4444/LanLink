#pragma once

#include "lanlink/protocol/network_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace lanlink::transport {

struct WintunNetworkAddress {
    protocol::NetworkId network_id{};
    std::uint32_t address = 0;
    std::uint32_t subnet = 0;
    std::uint8_t prefix_length = 24;

    bool operator==(const WintunNetworkAddress&) const = default;
};

class WintunAdapter {
public:
    using PacketHandler = std::function<void(std::span<const std::byte>)>;

    explicit WintunAdapter(std::wstring name = L"LanLink",
                           std::uint32_t mtu = 1400,
                           std::uint32_t ring_capacity = 4U * 1024U * 1024U);
    ~WintunAdapter();

    WintunAdapter(const WintunAdapter&) = delete;
    WintunAdapter& operator=(const WintunAdapter&) = delete;
    WintunAdapter(WintunAdapter&&) = delete;
    WintunAdapter& operator=(WintunAdapter&&) = delete;

    void configure(const WintunNetworkAddress& address);
    void remove(const protocol::NetworkId& network_id) noexcept;
    void start(PacketHandler handler);
    void inject(std::span<const std::byte> packet);
    void stop() noexcept;
    [[nodiscard]] std::exception_ptr pump_error() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
