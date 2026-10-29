#include "lanlink/auth/handshake.hpp"
#include "lanlink/network/linux_tap.hpp"
#include "lanlink/transport/packet_bridge.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace lanlink;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
auth::SignedDeviceKey authenticate(const auth::DeviceIdentity& device, const auth::AuthToken& token) {
    auth::ClientHandshake client(device, token);
    auth::ServerHandshake server(token, 1, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    require(client.handle_result(server.handle_proof(client.handle_challenge(server.handle_hello(client.begin(1))))),
        "TAP fixture authentication failed");
    return server.signed_device_key();
}
void endpoint(int channel, bool first, const transport::PacketIdentitySnapshot& identity,
    const transport::PacketIdentitySnapshot& peer, const auth::NetworkKey& key) {
    require(unshare(CLONE_NEWNET) == 0, "Linux TAP test requires permission to create network namespaces");
    protocol::NetworkId id{}; id[0] = std::byte{1};
    const auto own_ip = first ? 0x0a400001U : 0x0a400002U;
    const auto peer_ip = first ? 0x0a400002U : 0x0a400001U;
    const protocol::NetworkPeerState peers{id, 1, 0x0a400000U, 24, own_ip,
        {{peer.device_id, peer_ip, peer.signed_key}}, identity.device_id, 1};
    const transport::ActiveNetworkPacketState state{peers, {1, key}};
    network::LinuxTapAdapter tap("lltest0");
    transport::VirtualPacketBridge bridge([&](const protocol::EncryptedNetworkPacket& packet) {
        const auto bytes = protocol::encode_encrypted_network_packet(packet);
        require(send(channel, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size()),
            "encrypted Ethernet channel failed");
    }, [&](std::span<const std::byte> packet) { tap.inject(packet); });
    bridge.set_mtu(1400);
    bridge.synchronize(identity, std::span(&state, 1));
    tap.start([&](std::span<const std::byte> packet) { bridge.outbound(packet); });
    const network::VirtualNetworkAddress address{id, own_ip, 0x0a400000U, 24};
    tap.configure(address);
    const auto name = tap.name();
    require(if_nametoindex(name.c_str()) != 0, "kernel TAP interface was not created");
    {
        network::LinuxTapAdapter existing(name);
        bool rejected = false;
        try { existing.start([](auto) {}); } catch (const std::exception&) { rejected = true; }
        require(rejected, "existing TAP interface was attached");
        network::LinuxTapAdapter overlapping("lltest1");
        overlapping.start([](auto) {});
        rejected = false;
        try { overlapping.configure(address); } catch (const std::exception&) { rejected = true; }
        require(rejected, "overlapping TAP subnet was accepted");
    }
    std::atomic_bool receive_error = false;
    std::jthread receiver([&](std::stop_token stop) {
        try {
            std::array<std::byte, 65536> bytes{};
            while (!stop.stop_requested()) {
                pollfd descriptor{channel, POLLIN, 0};
                if (poll(&descriptor, 1, 100) <= 0) continue;
                const auto size = recv(channel, bytes.data(), bytes.size(), 0);
                require(size > 0, "encrypted Ethernet receive failed");
                bridge.inbound({peer.device_id, protocol::decode_encrypted_network_packet(
                    std::span(bytes).first(static_cast<std::size_t>(size)))});
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n'; receive_error = true;
        }
    });
    const auto udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    require(udp >= 0, "cannot create TAP UDP socket");
    const timeval timeout{10, 0};
    require(setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "UDP timeout failed");
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(47777);
    local.sin_addr.s_addr = htonl(own_ip);
    require(bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "cannot bind kernel UDP to TAP address");
    sockaddr_in remote = local; remote.sin_addr.s_addr = htonl(peer_ip);
    constexpr std::string_view message = "encrypted Ethernet across real Linux TAP devices";
    const auto transmit = [&] {
        require(sendto(udp, message.data(), message.size(), 0, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) ==
            static_cast<ssize_t>(message.size()), "kernel UDP send over TAP failed");
    };
    if (first) transmit();
    std::array<char, 128> received{};
    const auto size = recv(udp, received.data(), received.size(), 0);
    require(size == static_cast<ssize_t>(message.size()) &&
        std::string_view(received.data(), static_cast<std::size_t>(size)) == message,
        "ARP and kernel UDP round trip through encrypted TAP failed");
    if (!first) {
        transmit();
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
    }
    ::close(udp);
    receiver.request_stop(); receiver.join();
    require(!receive_error && !tap.pump_error(), "TAP pump or authenticated receiver failed");
    bridge.synchronize(std::nullopt, {});
    tap.remove(id);
    tap.stop();
    require(if_nametoindex(name.c_str()) == 0, "TAP interface survived shutdown");
    ::close(channel);
}
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("lanlink-tap-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(directory);
        const auto a = auth::DeviceIdentity::load_or_create(directory / "a");
        const auto b = auth::DeviceIdentity::load_or_create(directory / "b");
        const auto token = auth::AuthToken::from_secret("lanlink-isolated-tap-integration-token");
        const transport::PacketIdentitySnapshot first{a.device_id(), authenticate(a, token)};
        const transport::PacketIdentitySnapshot second{b.device_id(), authenticate(b, token)};
        const auto key = auth::NetworkKey::random();
        int channels[2]{};
        require(socketpair(AF_UNIX, SOCK_DGRAM, 0, channels) == 0, "cannot create encrypted test channel");
        std::array<pid_t, 2> children{};
        for (std::size_t index = 0; index < children.size(); ++index) {
            children[index] = fork();
            require(children[index] >= 0, "cannot create isolated TAP endpoint");
            if (children[index] == 0) {
                ::close(channels[1 - index]);
                try {
                    endpoint(channels[index], index == 0, index == 0 ? first : second,
                        index == 0 ? second : first, key);
                    _exit(0);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n' << std::flush; _exit(1);
                }
            }
        }
        ::close(channels[0]); ::close(channels[1]);
        bool passed = true;
        for (auto child : children) {
            int status = 0;
            passed = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && passed;
        }
        std::filesystem::remove_all(directory);
        require(passed, "isolated Linux TAP integration failed");
        std::cout << "Two real TAP interfaces: encrypted ARP/UDP, namespace isolation, conflict rejection and cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n'; return 1;
    }
}
