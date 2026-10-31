#pragma once
#include "lanlink/network/routed_network.hpp"
#include <memory>
namespace lanlink::transport {
// Configures only the LanLink interface. LAN forwarding must already be authorized
// and configured by the machine administrator. WFP restricts both forwarding directions.
class WindowsGateway {
public:
    explicit WindowsGateway(std::uint64_t interface_luid);
    ~WindowsGateway();
    WindowsGateway(const WindowsGateway&)=delete;
    WindowsGateway& operator=(const WindowsGateway&)=delete;
    void configure(std::span<const network::AdapterForward> forwards);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
