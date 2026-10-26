#include "lanlink/auth/handshake.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/network/virtual_network.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
using namespace lanlink;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class MemoryAdapter final : public network::VirtualAdapter {
public:
    void configure(const network::VirtualNetworkAddress& address) override { addresses[address.network_id] = address; }
    void remove(const protocol::NetworkId& id) noexcept override { addresses.erase(id); }
    void start(PacketHandler value) override { handler = std::move(value); running = true; }
    void inject(std::span<const std::byte> packet) override {
        if (running) received.emplace_back(packet.begin(), packet.end());
    }
    void stop() noexcept override { running = false; handler = {}; addresses.clear(); }
    std::exception_ptr pump_error() const noexcept override { return {}; }
    bool running = false;
    PacketHandler handler;
    std::map<protocol::NetworkId, network::VirtualNetworkAddress> addresses;
    std::vector<std::vector<std::byte>> received;
};

class MemoryPath final : public transport::PacketPath {
public:
    void start() override {
        if (fail_start) throw std::runtime_error("simulated path start failure");
        running = true;
    }
    void stop() noexcept override { running = false; }
    void set_receive_handler(ReceiveHandler value) override { receiver = std::move(value); }
    transport::PacketSendResult send(const protocol::EncryptedNetworkPacket& packet) override {
        if (!running) return transport::PacketSendResult::unavailable;
        sent.push_back(packet);
        if (other && other->receiver) other->receiver({identity, packet});
        return transport::PacketSendResult::accepted;
    }
    transport::PathStatus status() const override {
        return {transport::PathKind::relay_quic, running ? transport::PathLifecycle::authenticated
                                                       : transport::PathLifecycle::stopped, {}, 0, 0};
    }
    bool running = false, fail_start = false;
    auth::DeviceId identity{};
    MemoryPath* other = nullptr;
    ReceiveHandler receiver;
    std::vector<protocol::EncryptedNetworkPacket> sent;
};

auth::SignedDeviceKey authenticate(const auth::DeviceIdentity& device, const auth::AuthToken& token) {
    auth::ClientHandshake client(device, token);
    auth::ServerHandshake server(token, 1, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    require(client.handle_result(server.handle_proof(client.handle_challenge(server.handle_hello(client.begin(1))))),
            "fixture authentication failed");
    return server.signed_device_key();
}

void scenario(const std::filesystem::path& directory) {
    auto owner = auth::DeviceIdentity::load_or_create(directory / "owner");
    auto member = auth::DeviceIdentity::load_or_create(directory / "member");
    auto token = auth::AuthToken::from_secret("lanlink-independent-virtual-network-fixture");
    const auto own_key = authenticate(owner, token), peer_key = authenticate(member, token);
    const auto network_key = auth::NetworkKey::random();
    protocol::NetworkId id{}; id[0] = std::byte{1};
    const protocol::NetworkPeerState own_peers{id, 1, 0x0a400000U, 24, 0x0a400001U,
        {{member.device_id(), 0x0a400002U, peer_key}}, owner.device_id(), 1};
    const protocol::NetworkPeerState member_peers{id, 1, 0x0a400000U, 24, 0x0a400002U,
        {{owner.device_id(), 0x0a400001U, own_key}}, owner.device_id(), 1};
    const session::NetworkSnapshot own_state{{{owner.device_id(), own_key}}, {{own_peers, {1, network_key}}}};
    const session::NetworkSnapshot peer_state{{{member.device_id(), peer_key}}, {{member_peers, {1, network_key}}}};
    core::Logger logger(directory / "runtime.log", "test", core::LogLevel::warning, 1024 * 1024, 2);
    auto a = std::make_shared<MemoryAdapter>(), b = std::make_shared<MemoryAdapter>();
    MemoryPath outgoing, incoming;
    outgoing.identity = owner.device_id(); incoming.identity = member.device_id();
    outgoing.other = &incoming; incoming.other = &outgoing;
    network::VirtualNetwork first(a, outgoing, logger), second(b, incoming, logger);
    first.start(); second.start(); first.synchronize(own_state); second.synchronize(peer_state);
    require(a->addresses.at(id).address == 0x0a400001U && b->addresses.at(id).address == 0x0a400002U,
            "adapter did not receive the independent network snapshot");
    std::vector<std::byte> plain(24);
    plain[0] = std::byte{0x45}; plain[3] = std::byte{24}; plain[8] = std::byte{64}; plain[9] = std::byte{17};
    plain[12] = plain[16] = std::byte{10}; plain[13] = plain[17] = std::byte{64};
    plain[15] = std::byte{1}; plain[19] = std::byte{2};
    a->handler(plain);
    require(b->received.size() == 1 && b->received[0] == plain && outgoing.sent[0].ciphertext != plain,
            "independent adapters did not exchange an encrypted IPv4 packet");
    incoming.receiver({owner.device_id(), outgoing.sent[0]});
    require(b->received.size() == 1, "duplicate path delivery bypassed replay protection");
    require(second.rejected_packets() == 1, "rejected packet telemetry did not count the duplicate");
    first.synchronize(own_state); second.synchronize(peer_state); a->handler(plain);
    require(outgoing.sent.back().sequence == 2 && b->received.size() == 2,
            "snapshot refresh reset the cipher session");
    second.synchronize({});
    require(b->addresses.empty(), "revocation left an adapter address installed");
    a->handler(plain);
    require(b->received.size() == 2, "revocation left packet injection enabled");
    first.stop(); second.stop();
    require(!a->running && !b->running && !outgoing.running && !incoming.running && !incoming.receiver,
            "virtual network shutdown left a path or adapter active");
    MemoryPath unavailable; unavailable.fail_start = true;
    auto failed_adapter = std::make_shared<MemoryAdapter>();
    network::VirtualNetwork failed(failed_adapter, unavailable, logger);
    bool rejected = false;
    try { failed.start(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected && !failed_adapter->running && !unavailable.receiver,
            "path startup failure did not roll the adapter back");
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-virtual-network-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory); scenario(directory);
        std::filesystem::remove_all(directory); std::cout << "virtual network tests passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::error_code ignored;
        std::filesystem::remove_all(directory, ignored); return 1;
    }
}
