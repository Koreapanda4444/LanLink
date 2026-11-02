#include "lanlink/platform/adapter_factory.hpp"
#include "lanlink/transport/wintun_adapter.hpp"
namespace lanlink::platform {
std::shared_ptr<network::VirtualAdapter> make_virtual_adapter(std::uint16_t mtu) {
    return std::make_shared<transport::WintunAdapter>(L"LanLink", mtu);
}
}
