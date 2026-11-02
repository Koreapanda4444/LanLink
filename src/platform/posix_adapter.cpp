#include "lanlink/platform/adapter_factory.hpp"
#include "lanlink/network/posix_tun.hpp"
namespace lanlink::platform {
std::shared_ptr<network::VirtualAdapter> make_virtual_adapter(std::uint16_t mtu) {
    return std::make_shared<network::PosixTunAdapter>(mtu);
}
}
