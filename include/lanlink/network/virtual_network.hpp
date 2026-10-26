#pragma once

#include "lanlink/network/virtual_adapter.hpp"
#include "lanlink/session/network_state.hpp"
#include "lanlink/transport/packet_path.hpp"

#include <map>
#include <memory>
#include <mutex>

namespace lanlink::core { class Logger; }

namespace lanlink::network {

class VirtualNetwork {
public:
    VirtualNetwork(std::shared_ptr<VirtualAdapter> adapter,
                   transport::PacketPath& path, core::Logger& logger);
    ~VirtualNetwork();
    VirtualNetwork(const VirtualNetwork&) = delete;
    VirtualNetwork& operator=(const VirtualNetwork&) = delete;
    void start();
    void synchronize(const session::NetworkSnapshot& snapshot);
    void stop() noexcept;
    [[nodiscard]] std::exception_ptr pump_error() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
    transport::PacketPath& path_;
    std::mutex lifecycle_mutex_;
    bool started_ = false;
    std::map<protocol::NetworkId, VirtualNetworkAddress> installed_;
};

}
