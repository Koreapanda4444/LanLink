#include "lanlink/auth/identity.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/packet_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace protocol = lanlink::protocol;
namespace transport = lanlink::transport;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void wait_for(Predicate predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(10ms);
    }
}

transport::QuicClientOptions options(const std::filesystem::path& root,
                                    const std::string& name, std::uint16_t port) {
    transport::QuicClientOptions result;
    result.relay_host = "127.0.0.1";
    result.port = port;
    result.identity_file = root / (name + ".identity");
    result.auth_token_file = root / "auth.token";
    result.handshake_timeout = 500ms;
    result.authentication_timeout = 2s;
    result.reconnect_initial_delay = 50ms;
    result.reconnect_maximum_delay = 150ms;
    result.tcp_probe_interval = 60s;
    return result;
}

class Node {
public:
    Node(const std::filesystem::path& root, const std::string& name, std::uint16_t port)
        : identity(lanlink::auth::DeviceIdentity::load_or_create(root / (name + ".identity"))),
          logger(root / (name + ".log"), name, lanlink::core::LogLevel::info, 1024 * 1024, 2),
          bridge([this](const protocol::EncryptedNetworkPacket& packet) {
              {
                  std::lock_guard lock(mutex);
                  last_sent = packet;
              }
              client.send_encrypted_packet(packet);
          }, [this](std::span<const std::byte> packet) {
              std::lock_guard lock(mutex);
              received.emplace_back(packet.begin(), packet.end());
          }), client(options(root, name, port), logger) {
        client.set_encrypted_packet_handler([this](const protocol::ForwardedNetworkPacket& packet) {
            try {
                {
                    std::lock_guard lock(mutex);
                    ++deliveries;
                    last_received = packet;
                }
                synchronize();
                bridge.inbound(packet);
            } catch (...) {
                std::lock_guard lock(mutex);
                error = std::current_exception();
            }
        });
        worker = std::jthread([this](std::stop_token token) {
            try {
                client.run(token);
            } catch (...) {
                std::lock_guard lock(mutex);
                error = std::current_exception();
            }
        });
    }

    ~Node() {
        stop();
    }

    void stop() {
        worker.request_stop();
        client.stop();
        if (worker.joinable()) worker.join();
    }

    void synchronize() {
        std::vector<transport::ActiveNetworkPacketState> networks;
        for (const auto& state : client.cached_peer_states()) {
            const auto key = client.cached_network_key(state.network_id);
            if (key && key->epoch == state.key_epoch) {
                networks.push_back({state, *key});
            }
        }
        bridge.synchronize(client.cached_packet_identity(), networks);
    }

    void send(const std::vector<std::byte>& packet) {
        check();
        synchronize();
        bridge.outbound(packet);
    }

    void check() {
        std::lock_guard lock(mutex);
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::size_t count() {
        check();
        std::lock_guard lock(mutex);
        return received.size();
    }

    void expect_packet(std::size_t index, const std::vector<std::byte>& packet) {
        wait_for([&] { return count() > index; }, "encrypted packet was not injected");
        std::lock_guard lock(mutex);
        require(received.at(index) == packet, "injected IPv4 packet differs from original");
    }

    std::size_t delivered() {
        check();
        std::lock_guard lock(mutex);
        return deliveries;
    }

    lanlink::auth::DeviceIdentity identity;
    lanlink::core::Logger logger;
    std::mutex mutex;
    std::vector<std::vector<std::byte>> received;
    std::size_t deliveries = 0;
    std::exception_ptr error;
    protocol::EncryptedNetworkPacket last_sent;
    protocol::ForwardedNetworkPacket last_received;
    transport::VirtualPacketBridge bridge;
    transport::QuicRelayClient client;
    std::jthread worker;
};

std::vector<std::byte> ipv4(std::uint32_t source, std::uint32_t destination,
                            std::size_t size, unsigned char marker) {
    require(size >= 28 && size <= 65535, "invalid fixture packet size");
    std::vector<std::byte> packet(size, std::byte{marker});
    std::fill_n(packet.begin(), 28, std::byte{0});
    const auto word = [&](std::size_t offset, std::uint16_t value) {
        packet[offset] = std::byte{static_cast<unsigned char>(value >> 8U)};
        packet[offset + 1] = std::byte{static_cast<unsigned char>(value)};
    };
    const auto address = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) {
            packet[offset + i] = std::byte{static_cast<unsigned char>(value >> (24U - 8U * i))};
        }
    };
    packet[0] = std::byte{0x45};
    packet[8] = std::byte{64};
    packet[9] = std::byte{17};
    word(2, static_cast<std::uint16_t>(size));
    word(4, marker);
    address(12, source);
    address(16, destination);
    word(20, 4242);
    word(22, 5353);
    word(24, static_cast<std::uint16_t>(size - 20));
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < 20; i += 2) {
        checksum += (std::to_integer<std::uint32_t>(packet[i]) << 8U) |
                    std::to_integer<std::uint32_t>(packet[i + 1]);
    }
    while (checksum >> 16U) {
        checksum = (checksum & 0xffffU) + (checksum >> 16U);
    }
    word(10, static_cast<std::uint16_t>(~checksum));
    return packet;
}

void relay_command(const std::filesystem::path& root, unsigned sequence,
                   const std::string& command) {
    {
        std::ofstream output(root / "request.tmp");
        output << sequence << ' ' << command;
        require(output.good(), "cannot write relay restart command");
    }
    std::filesystem::rename(root / "request.tmp", root / "request");
    wait_for([&] {
        unsigned completed = 0;
        std::ifstream(root / "completed") >> completed;
        return completed == sequence;
    }, "relay process restart command timed out");
}

void ready(Node& owner, Node& member, const protocol::NetworkId& id) {
    wait_for([&] {
        owner.check();
        member.check();
        const auto a = owner.client.cached_peer_state(id);
        const auto b = member.client.cached_peer_state(id);
        const auto ka = owner.client.cached_network_key(id);
        const auto kb = member.client.cached_network_key(id);
        return a && b && a->peers.size() == 1 && b->peers.size() == 1 &&
               a->peers.front().signed_key && b->peers.front().signed_key && ka && kb &&
               *ka == *kb && ka->epoch == a->key_epoch && kb->epoch == b->key_epoch;
    }, "approved clients did not receive matching keys and signed peer state");
    owner.synchronize();
    member.synchronize();
}

void scenario(const std::filesystem::path& root, std::uint16_t port, bool tcp) {
    Node owner(root, "owner", port);
    Node member(root, "member", port);
    Node outsider(root, "outsider", port);
    wait_for([&] {
        return owner.client.authenticated() && member.client.authenticated() &&
               outsider.client.authenticated();
    }, "three real clients did not authenticate against the relay executable");
    require(owner.client.using_tcp_fallback() == tcp &&
                member.client.using_tcp_fallback() == tcp &&
                outsider.client.using_tcp_fallback() == tcp,
            "clients selected the wrong transport");
    const auto created = owner.client.create_network("E2E LAN");
    require(created.code == protocol::NetworkResultCode::success, "network creation failed");
    const auto id = created.network_id;
    require(member.client.join_network(id).code == protocol::NetworkResultCode::pending_approval,
            "unapproved member must wait for the owner");
    require(owner.client.approve_member(id, member.identity.device_id()).code ==
                protocol::NetworkResultCode::success, "member approval failed");
    require(outsider.client.create_network("Isolated LAN").code ==
                protocol::NetworkResultCode::success, "isolated network creation failed");
    ready(owner, member, id);
    const auto owner_ip = owner.client.cached_peer_state(id)->own_address;
    const auto member_ip = member.client.cached_peer_state(id)->own_address;
    const auto old_key = *owner.client.cached_network_key(id);
    const auto old_identity = *owner.client.cached_packet_identity();

    if (!tcp) {
        wait_for([&] { return owner.client.max_datagram_size() > 0 &&
                              member.client.max_datagram_size() > 0; },
                 "QUIC datagram support did not become available");
    }
    auto packet = ipv4(owner_ip, member_ip, 128, 1);
    auto datagrams = member.client.received_datagram_count();
    owner.send(packet);
    member.expect_packet(0, packet);
    require(tcp || member.client.received_datagram_count() > datagrams,
            "small encrypted IPv4 packet did not use a QUIC datagram");
    require(owner.last_sent.ciphertext != packet, "relay packet contains plaintext IPv4");
    packet = ipv4(member_ip, owner_ip, 128, 2);
    member.send(packet);
    owner.expect_packet(0, packet);
    datagrams = member.client.received_datagram_count();
    packet = ipv4(owner_ip, member_ip, 8192, 3);
    owner.send(packet);
    member.expect_packet(1, packet);
    require(member.client.received_datagram_count() == datagrams,
            "large IPv4 packet must use the reliable stream");
    bool rejected = false;
    try {
        member.bridge.inbound(member.last_received);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && member.count() == 2, "replayed ciphertext must not reach the adapter");

    std::size_t index = 2;
    for (const auto destination : {0xffffffffU, (owner_ip & 0xffffff00U) | 255U,
                                   0xe00000fbU}) {
        packet = ipv4(owner_ip, destination, 128, static_cast<unsigned char>(index + 2));
        owner.send(packet);
        member.expect_packet(index++, packet);
    }
    std::this_thread::sleep_for(200ms);
    require(outsider.delivered() == 0 && owner.count() == 1 && member.count() == 5,
            "broadcast/multicast leaked, reflected, or duplicated");
    const auto owner_count = owner.count();
    packet = ipv4(owner_ip, 0x08080808U, 128, 9);
    owner.send(packet);
    require(owner.count() == owner_count && member.count() == 5,
            "ordinary Internet traffic entered the virtual network");

    relay_command(root, 1, "stop");
    wait_for([&] { return !owner.client.authenticated() && !member.client.authenticated() &&
                          !owner.client.cached_network_key(id) &&
                          !member.client.cached_peer_state(id); },
             "disconnect did not clear authentication and cached network secrets");
    relay_command(root, 2, "start");
    wait_for([&] { return owner.client.authenticated() && member.client.authenticated() &&
                          outsider.client.authenticated(); }, "clients did not reconnect");
    ready(owner, member, id);
    require(owner.client.cached_peer_state(id)->own_address == owner_ip &&
                member.client.cached_peer_state(id)->own_address == member_ip,
            "relay restart did not preserve SQLite virtual IP leases");
    require(owner.client.cached_network_key(id)->key != old_key.key &&
                owner.client.cached_packet_identity()->signed_key != old_identity.signed_key,
            "reconnection must establish fresh session and network keys");
    const auto networks = owner.client.list_networks();
    require(networks.code == protocol::NetworkResultCode::success &&
                networks.result.networks.size() == 1, "network membership did not survive restart");
    packet = ipv4(owner_ip, member_ip, 128, 10);
    owner.send(packet);
    member.expect_packet(5, packet);

    require(owner.client.kick_member(id, member.identity.device_id()).code ==
                protocol::NetworkResultCode::success, "member eviction failed");
    wait_for([&] { return !member.client.cached_peer_state(id) &&
                          !member.client.cached_network_key(id); },
             "evicted client retained peer state or network keys");
    wait_for([&] {
        const auto state = owner.client.cached_peer_state(id);
        const auto key = owner.client.cached_network_key(id);
        return state && state->peers.empty() && key && key->epoch == state->key_epoch;
    }, "owner did not rotate the key after eviction");
    const auto delivered = member.delivered();
    owner.send(ipv4(owner_ip, 0xffffffffU, 128, 11));
    std::this_thread::sleep_for(200ms);
    require(member.delivered() == delivered && outsider.delivered() == 0,
            "relay routed packets to an evicted or unrelated device");
    owner.check();
    member.check();
    outsider.check();
    Node next(root, "next", port);
    std::this_thread::sleep_for(600ms);
    require(!next.client.authenticated(), "fourth client bypassed the shared connection limit");
    require(owner.client.list_networks().code == protocol::NetworkResultCode::success,
            "connection rejection disrupted an existing client's control channel");
    outsider.stop();
    wait_for([&] { return next.client.authenticated(); },
             "closing a client did not release the shared connection slot");
    require(next.client.list_networks().code == protocol::NetworkResultCode::success,
            "client admitted after capacity recovery cannot use the control channel");
    std::cout << (tcp ? "TLS fallback" : "QUIC")
              << ": encryption, unicast, stream, fanout, isolation, restart, eviction, limits passed\n";
}

}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    try {
        require(argc == 4, "usage: relay-e2e fixture port quic|tcp");
        const auto port = std::stoul(argv[2]);
        require(port > 0 && port <= 65535, "invalid port");
        scenario(argv[1], static_cast<std::uint16_t>(port), std::string(argv[3]) == "tcp");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "relay E2E: " << error.what() << '\n';
        return 1;
    }
}
