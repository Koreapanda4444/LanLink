#include "lanlink/network/virtual_network.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/packet_bridge.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace lanlink::network {

struct VirtualNetwork::State {
    std::shared_ptr<VirtualAdapter> adapter;
    std::shared_ptr<transport::VirtualPacketBridge> bridge;
    core::Logger& logger;
    std::atomic<std::int64_t> last_warning_ms = 0;
    std::atomic<std::uint64_t> rejected_packets = 0;

    State(std::shared_ptr<VirtualAdapter> value, core::Logger& log)
        : adapter(std::move(value)), logger(log) {}

    void warn(const char* operation) {
        rejected_packets.fetch_add(1);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto previous = last_warning_ms.load();
        if (now - previous >= 5000 && last_warning_ms.compare_exchange_strong(previous, now))
            logger.warning(operation);
    }
};

VirtualNetwork::VirtualNetwork(std::shared_ptr<VirtualAdapter> adapter,
                               transport::PacketPath& path, core::Logger& logger)
    : state_(std::make_shared<State>(std::move(adapter), logger)), path_(path) {
    if (!state_->adapter) throw std::invalid_argument("virtual network needs an adapter");
    const auto device = state_->adapter;
    state_->bridge = std::make_shared<transport::VirtualPacketBridge>(
        [&path](const protocol::EncryptedNetworkPacket& packet) {
            if (path.send(packet) != transport::PacketSendResult::accepted)
                throw std::runtime_error("encrypted packet path unavailable");
        }, [device](std::span<const std::byte> packet) { device->inject(packet); });
    state_->bridge->set_mtu(device->packet_mtu());
}

VirtualNetwork::~VirtualNetwork() { stop(); }

void VirtualNetwork::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (started_) throw std::logic_error("virtual network is already started");
    const std::weak_ptr<State> weak = state_;
    path_.set_receive_handler([weak](const protocol::ForwardedNetworkPacket& packet) {
        if (const auto state = weak.lock()) {
            try { state->bridge->inbound(packet); }
            catch (const std::exception&) { state->warn("incoming virtual packet rejected"); }
        }
    });
    try {
        state_->adapter->start([weak](std::span<const std::byte> packet) {
            if (const auto state = weak.lock()) {
                try { state->bridge->outbound(packet); }
                catch (const std::exception&) { state->warn("outgoing virtual packet dropped"); }
            }
        });
        path_.start();
        started_ = true;
    } catch (...) {
        state_->adapter->stop();
        state_->bridge->clear();
        path_.set_receive_handler({});
        path_.stop();
        throw;
    }
}

void VirtualNetwork::synchronize(const session::NetworkSnapshot& snapshot) {
    std::lock_guard lock(lifecycle_mutex_);
    if (!started_) throw std::logic_error("virtual network is stopped");
    std::map<protocol::NetworkId, VirtualNetworkAddress> desired;
    if (snapshot.identity) for (const auto& network : snapshot.networks) {
        const auto& p = network.peers;
        const VirtualNetworkAddress address{p.network_id, p.own_address, p.subnet_address, p.prefix_length};
        if (!desired.emplace(p.network_id, address).second)
            throw std::invalid_argument("duplicate virtual network address");
    }
    for (const auto& [id, address] : desired) {
        state_->adapter->configure(address);
        installed_.insert_or_assign(id, address);
    }
    state_->bridge->synchronize(snapshot.identity, snapshot.networks);
    state_->bridge->synchronize_multicast(state_->adapter->multicast_groups());
    for (auto it = installed_.begin(); it != installed_.end();) {
        if (!desired.contains(it->first)) {
            state_->adapter->remove(it->first);
            it = installed_.erase(it);
        } else ++it;
    }
}

void VirtualNetwork::stop() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    if (!started_) return;
    state_->adapter->stop();
    state_->bridge->clear();
    try { path_.set_receive_handler({}); } catch (...) {}
    path_.stop();
    installed_.clear();
    started_ = false;
}

std::exception_ptr VirtualNetwork::pump_error() const noexcept { return state_->adapter->pump_error(); }
std::uint64_t VirtualNetwork::rejected_packets() const noexcept { return state_->rejected_packets.load(); }

protocol::GameDiagnostics VirtualNetwork::game_diagnostics() const {
    auto result = state_->bridge->game_diagnostics();
    result.rejected_packets = state_->rejected_packets.load();
    result.adapter_failed = static_cast<bool>(state_->adapter->pump_error());
    return result;
}

}
