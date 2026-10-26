#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <netioapi.h>

#include "lanlink/auth/identity.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/local_pipe.hpp"
#include "lanlink/transport/packet_bridge.hpp"
#include "lanlink/transport/quic.hpp"
#include "lanlink/transport/tls.hpp"
#include "lanlink/transport/wintun_adapter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace auth = lanlink::auth;
namespace protocol = lanlink::protocol;
namespace transport = lanlink::transport;
constexpr std::uint32_t mapped_owner = 0x0afefd01U;
constexpr std::uint32_t mapped_peer = 0x0afefd02U;

void require(bool value, const std::string& description) {
    if (!value) throw std::runtime_error(description + " (Windows error " + std::to_string(GetLastError()) + ")");
}

template <typename Predicate>
void wait_for(Predicate predicate, const std::string& description) {
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, description);
        std::this_thread::sleep_for(25ms);
    }
}

class Socket {
public:
    explicit Socket(SOCKET value = INVALID_SOCKET) : value_(value) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(other.value_), closed_(other.closed_.exchange(true)) {}
    [[nodiscard]] SOCKET get() const noexcept { return value_; }
    void close() noexcept {
        if (!closed_.exchange(true) && value_ != INVALID_SOCKET) {
            shutdown(value_, SD_BOTH);
            closesocket(value_);
        }
    }
private:
    SOCKET value_;
    std::atomic_bool closed_ = false;
};

sockaddr_in endpoint(std::uint32_t address, std::uint16_t port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(address);
    result.sin_port = htons(port);
    return result;
}

void timeouts(const Socket& socket) {
    const DWORD timeout = 5000;
    for (const auto option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
        require(setsockopt(socket.get(), SOL_SOCKET, option, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
                "cannot set socket timeout");
    }
}

Socket bound_socket(int type, std::uint32_t address, std::uint16_t port = 0) {
    Socket socket(::socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP));
    require(socket.get() != INVALID_SOCKET, "cannot create application socket");
    timeouts(socket);
    const auto local = endpoint(address, port);
    require(bind(socket.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0,
            "cannot bind application socket");
    return socket;
}

std::uint16_t port_of(const Socket& socket) {
    sockaddr_in local{};
    int size = sizeof(local);
    require(getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &size) == 0, "cannot read socket port");
    return ntohs(local.sin_port);
}

void send_all(const Socket& socket, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = send(socket.get(), reinterpret_cast<const char*>(bytes.data()),
                                static_cast<int>(bytes.size()), 0);
        require(count > 0, "TCP application send failed");
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
}

std::vector<std::byte> receive_all(const Socket& socket, std::size_t size) {
    std::vector<std::byte> result(size);
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = recv(socket.get(), reinterpret_cast<char*>(result.data() + offset),
                                static_cast<int>(size - offset), 0);
        require(count > 0, "TCP application receive failed");
        offset += static_cast<std::size_t>(count);
    }
    return result;
}

class TcpOnlyProxy {
    struct Pair {
        Pair(Socket inbound, Socket outbound) : inbound(std::move(inbound)), outbound(std::move(outbound)) {}
        Socket inbound;
        Socket outbound;
    };
public:
    TcpOnlyProxy(std::uint16_t port, std::uint16_t target)
        : listener_(bound_socket(SOCK_STREAM, 0x7f000001U, port)), target_(target) {
        require(listen(listener_.get(), 8) == 0, "cannot listen on TCP-only proxy");
        acceptor_ = std::jthread([this](std::stop_token token) {
            while (!token.stop_requested()) {
                fd_set readers;
                FD_ZERO(&readers);
                FD_SET(listener_.get(), &readers);
                timeval delay{0, 100000};
                if (select(0, &readers, nullptr, nullptr, &delay) <= 0) continue;
                try {
                    Socket inbound(accept(listener_.get(), nullptr, nullptr));
                    require(inbound.get() != INVALID_SOCKET, "proxy accept failed");
                    Socket outbound(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                    timeouts(inbound);
                    timeouts(outbound);
                    const auto remote = endpoint(0x7f000001U, target_);
                    require(connect(outbound.get(), reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0,
                            "proxy upstream connection failed");
                    auto pair = std::make_shared<Pair>(std::move(inbound), std::move(outbound));
                    connections_.push_back(pair);
                    workers_.emplace_back([this, pair](std::stop_token) {
                        std::array<std::byte, 16384> bytes{};
                        try {
                            while (!stopping_.load()) {
                                fd_set input;
                                FD_ZERO(&input);
                                FD_SET(pair->inbound.get(), &input);
                                FD_SET(pair->outbound.get(), &input);
                                timeval timeout{0, 100000};
                                if (select(0, &input, nullptr, nullptr, &timeout) <= 0) continue;
                                for (int direction = 0; direction < 2; ++direction) {
                                    auto& source = direction == 0 ? pair->inbound : pair->outbound;
                                    auto& target = direction == 0 ? pair->outbound : pair->inbound;
                                    if (!FD_ISSET(source.get(), &input)) continue;
                                    const auto size = recv(source.get(), reinterpret_cast<char*>(bytes.data()),
                                                           static_cast<int>(bytes.size()), 0);
                                    if (size <= 0) throw std::runtime_error("proxy peer closed");
                                    send_all(target, std::span(bytes).first(static_cast<std::size_t>(size)));
                                }
                            }
                        } catch (const std::exception&) {}
                        pair->inbound.close();
                        pair->outbound.close();
                    });
                } catch (const std::exception&) {}
            }
        });
    }
    ~TcpOnlyProxy() {
        stopping_.store(true);
        acceptor_.request_stop();
        listener_.close();
        acceptor_.join();
        for (auto& pair : connections_) { pair->inbound.close(); pair->outbound.close(); }
        workers_.clear();
    }
private:
    Socket listener_;
    std::uint16_t target_;
    std::atomic_bool stopping_ = false;
    std::vector<std::shared_ptr<Pair>> connections_;
    std::vector<std::jthread> workers_;
    std::jthread acceptor_;
};

std::uint32_t address_at(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + i]);
    return value;
}

void set_address(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = std::byte{static_cast<unsigned char>(value >> (24U - 8U * i))};
}

void set_checksum(std::span<std::byte> bytes, std::size_t offset, std::uint32_t sum = 0) {
    bytes[offset] = bytes[offset + 1] = std::byte{0};
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        sum += std::to_integer<std::uint32_t>(bytes[i]) << 8U;
        if (i + 1 < bytes.size()) sum += std::to_integer<std::uint32_t>(bytes[i + 1]);
    }
    while (sum >> 16U) sum = (sum & 0xffffU) + (sum >> 16U);
    const auto checksum = static_cast<std::uint16_t>(~sum);
    bytes[offset] = std::byte{static_cast<unsigned char>(checksum >> 8U)};
    bytes[offset + 1] = std::byte{static_cast<unsigned char>(checksum)};
}

std::vector<std::byte> translate(std::span<const std::byte> input, std::uint32_t source, std::uint32_t destination) {
    require(input.size() >= 20 && (std::to_integer<unsigned>(input[0]) >> 4U) == 4, "expected an IPv4 packet");
    std::vector<std::byte> bytes(input.begin(), input.end());
    const auto header = static_cast<std::size_t>(std::to_integer<unsigned>(bytes[0]) & 15U) * 4U;
    require(header >= 20 && header <= bytes.size(), "invalid IPv4 header");
    require((std::to_integer<unsigned>(bytes[6]) & 0x3fU) == 0 && bytes[7] == std::byte{0},
            "application fixture unexpectedly generated fragmented IPv4");
    set_address(bytes, 12, source);
    set_address(bytes, 16, destination);
    set_checksum(std::span(bytes).first(header), 10);
    const auto protocol = std::to_integer<unsigned>(bytes[9]);
    auto payload = std::span(bytes).subspan(header);
    if (protocol == IPPROTO_TCP) {
        require(payload.size() >= 20, "invalid TCP segment");
        const auto sum = (source >> 16U) + (source & 65535U) + (destination >> 16U) +
                         (destination & 65535U) + IPPROTO_TCP + static_cast<std::uint32_t>(payload.size());
        set_checksum(payload, 16, sum);
    } else if (protocol == IPPROTO_UDP) {
        require(payload.size() >= 8, "invalid UDP datagram");
        payload[6] = payload[7] = std::byte{0};
    }
    return bytes;
}

transport::QuicClientOptions client_options(const std::filesystem::path& root, const std::string& name,
                                          std::uint16_t port) {
    transport::QuicClientOptions result;
    result.relay_host = "127.0.0.1";
    result.port = port;
    result.identity_file = root / (name + ".identity");
    result.auth_token_file = root / "auth.token";
    result.handshake_timeout = 1000ms;
    result.authentication_timeout = 5s;
    result.idle_timeout = 5s;
    result.keep_alive_interval_ms = 1000;
    result.reconnect_initial_delay = 100ms;
    result.reconnect_maximum_delay = 500ms;
    return result;
}

class Peer {
public:
    Peer(const std::filesystem::path& root, const std::string& name, std::uint16_t port, bool with_adapter = false)
        : identity(auth::DeviceIdentity::load_or_create(root / (name + ".identity"))),
          logger(root / (name + ".log"), name, lanlink::core::LogLevel::info, 1024 * 1024, 2),
          adapter_(with_adapter ? std::make_unique<transport::WintunAdapter>(
              L"LanLink-E2E-Peer-" + std::to_wstring(GetCurrentProcessId())) : nullptr),
          bridge([this](const protocol::EncryptedNetworkPacket& packet) { client.send_encrypted_packet(packet); },
                 [this](std::span<const std::byte> bytes) {
                     const auto destination = address_at(bytes, 16);
                     if (destination != virtual_peer_.load()) {
                         const auto header = static_cast<std::size_t>(std::to_integer<unsigned>(bytes[0]) & 15U) * 4U;
                         const std::array<char, 7> marker{'L', 'a', 'n', 'L', 'i', 'n', 'k'};
                         if (bytes[9] == std::byte{IPPROTO_UDP} && header >= 20 && bytes.size() == header + 40 &&
                             std::equal(marker.begin(), marker.end(), bytes.begin() + header + 8,
                                 [](char expected, std::byte actual) { return std::byte{static_cast<unsigned char>(expected)} == actual; })) {
                             fanout_.fetch_add(1);
                         }
                         return;
                     }
                     if (adapter_ && injection_ready_.load()) adapter_->inject(translate(bytes, mapped_owner, mapped_peer));
                 }), client(client_options(root, name, port), logger) {
        client.set_encrypted_packet_handler([this](const protocol::ForwardedNetworkPacket& packet) {
            deliveries_.fetch_add(1);
            try { synchronize(); bridge.inbound(packet); }
            catch (const std::exception& error) { logger.warning(std::string("fixture incoming packet dropped: ") + error.what()); }
        });
        worker = std::jthread([this](std::stop_token token) {
            try { client.run(token); }
            catch (...) { std::lock_guard lock(mutex_); error_ = std::current_exception(); }
        });
    }
    ~Peer() {
        closing_.store(true);
        injection_ready_.store(false);
        if (adapter_) adapter_->stop();
        client.set_encrypted_packet_handler({});
        worker.request_stop();
        client.stop();
        worker.join();
    }
    void synchronize() {
        std::vector<transport::ActiveNetworkPacketState> networks;
        for (const auto& state : client.cached_peer_states()) {
            const auto key = client.cached_network_key(state.network_id);
            if (key && key->epoch == state.key_epoch) networks.push_back({state, *key});
        }
        bridge.synchronize(client.cached_packet_identity(), networks);
    }
    void attach(const protocol::NetworkId& id) {
        const auto state = client.cached_peer_state(id);
        require(state && state->peers.size() == 1, "peer network is incomplete");
        virtual_peer_.store(state->own_address);
        virtual_owner_.store(state->peers.front().ipv4_address);
        synchronize();
        require(adapter_ != nullptr, "peer adapter is missing");
        adapter_->configure({id, mapped_peer, mapped_peer & 0xffffff00U, 24});
        adapter_->start([this](std::span<const std::byte> bytes) {
            if (bytes.size() < 20 || address_at(bytes, 12) != mapped_peer || address_at(bytes, 16) != mapped_owner) return;
            try {
                synchronize();
                bridge.outbound(translate(bytes, virtual_peer_.load(), virtual_owner_.load()));
            } catch (const std::exception& error) {
                if (!closing_.load()) logger.warning(std::string("fixture outgoing packet dropped: ") + error.what());
            }
        });
        injection_ready_.store(true);
    }
    void check() {
        std::lock_guard lock(mutex_);
        if (error_) std::rethrow_exception(error_);
        if (adapter_ && adapter_->pump_error()) std::rethrow_exception(adapter_->pump_error());
    }
    std::size_t deliveries() const noexcept { return deliveries_.load(); }
    std::size_t fanout() const noexcept { return fanout_.load(); }
    auth::DeviceIdentity identity;
    lanlink::core::Logger logger;
private:
    std::mutex mutex_;
    std::exception_ptr error_;
    std::atomic_bool closing_ = false;
    std::atomic_bool injection_ready_ = false;
    std::atomic<std::uint32_t> virtual_peer_ = 0;
    std::atomic<std::uint32_t> virtual_owner_ = 0;
    std::atomic<std::size_t> deliveries_ = 0;
    std::atomic<std::size_t> fanout_ = 0;
    std::unique_ptr<transport::WintunAdapter> adapter_;
    transport::VirtualPacketBridge bridge;
public:
    transport::QuicRelayClient client;
    std::jthread worker;
};

std::optional<NET_LUID> address_luid(std::uint32_t address) {
    MIB_UNICASTIPADDRESS_TABLE* table = nullptr;
    require(GetUnicastIpAddressTable(AF_INET, &table) == NO_ERROR, "cannot inspect Windows virtual addresses");
    std::optional<NET_LUID> result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& row = table->Table[i];
        if (ntohl(row.Address.Ipv4.sin_addr.s_addr) == address && row.DadState == IpDadStatePreferred) result = row.InterfaceLuid;
    }
    FreeMibTable(table);
    return result;
}

std::vector<std::byte> local(protocol::LocalCommand command, std::vector<std::byte> payload = {}) {
    auto response = transport::local_pipe_request({command, false, false, std::move(payload)});
    require(!response.error, "Windows service rejected the local control request");
    return response.payload;
}

protocol::LocalDiagnostics diagnostics() {
    return protocol::decode_local_diagnostics(local(protocol::LocalCommand::diagnostics));
}

void command(const std::filesystem::path& root, unsigned sequence, const std::string& action) {
    const auto name = "request-" + std::to_string(sequence);
    {
        std::ofstream output(root / (name + ".tmp"));
        output << sequence << ' ' << action;
        require(output.good(), "cannot request process restart");
    }
    std::filesystem::rename(root / (name + ".tmp"), root / name);
    wait_for([&] { unsigned done = 0; std::ifstream(root / "completed") >> done; return done == sequence; },
             "Windows process control timed out");
}

void ready(Peer& peer, const protocol::NetworkId& id, std::optional<bool> tcp) {
    wait_for([&] {
        peer.check();
        const auto state = peer.client.cached_peer_state(id);
        const auto key = peer.client.cached_network_key(id);
        const auto service = diagnostics();
        return state && state->peers.size() == 1 && state->peers.front().signed_key && key &&
               key->epoch == state->key_epoch && service.authenticated && service.active_networks == 1 &&
               (!tcp || (service.tcp_fallback == *tcp && peer.client.using_tcp_fallback() == *tcp));
    }, "Windows service and peer did not restore authenticated network state");
    peer.synchronize();
}

void application_roundtrip(Peer& peer, std::uint32_t owner_ip, std::uint32_t peer_ip) {
    try {
        wait_for([&] { return address_luid(owner_ip).has_value() && address_luid(mapped_peer).has_value(); },
                 "Wintun virtual addresses did not become usable");
    } catch (...) {
        MIB_UNICASTIPADDRESS_TABLE* table = nullptr;
        if (GetUnicastIpAddressTable(AF_INET, &table) == NO_ERROR) {
            for (ULONG i = 0; i < table->NumEntries; ++i) {
                const auto& row = table->Table[i];
                const auto address = ntohl(row.Address.Ipv4.sin_addr.s_addr);
                if (address == owner_ip || address == mapped_peer) {
                    std::cerr << "Virtual address " << address << " interface " << row.InterfaceLuid.Value
                              << " DAD state " << row.DadState << '\n';
                }
            }
            FreeMibTable(table);
        }
        throw;
    }
    for (const auto address : {owner_ip, mapped_peer}) {
        MIB_IPINTERFACE_ROW row;
        InitializeIpInterfaceEntry(&row);
        row.Family = AF_INET;
        const auto luid = address_luid(address);
        require(luid.has_value(), "virtual interface disappeared before the socket test");
        row.InterfaceLuid = *luid;
        require(GetIpInterfaceEntry(&row) == NO_ERROR && row.DadTransmits == 0,
                "tunnel addresses must use relay-managed uniqueness");
    }
    const std::vector<std::byte> datagram(256, std::byte{0x5a});
    auto receiver = bound_socket(SOCK_DGRAM, mapped_peer);
    auto sender = bound_socket(SOCK_DGRAM, owner_ip);
    const auto destination = endpoint(peer_ip, port_of(receiver));
    const DWORD udp_timeout = 500;
    for (const auto* socket : {&receiver, &sender}) {
        require(setsockopt(socket->get(), SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&udp_timeout), sizeof(udp_timeout)) == 0, "cannot set UDP retry interval");
    }
    wait_for([&] {
        require(sendto(sender.get(), reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                       reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == static_cast<int>(datagram.size()),
                "Windows UDP application send failed");
        std::array<std::byte, 2048> bytes{};
        sockaddr_in remote{};
        int remote_size = sizeof(remote);
        const auto received = recvfrom(receiver.get(), reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                                        reinterpret_cast<sockaddr*>(&remote), &remote_size);
        peer.check();
        if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) return false;
        require(received == static_cast<int>(datagram.size()) && std::equal(datagram.begin(), datagram.end(), bytes.begin()) &&
                    ntohl(remote.sin_addr.s_addr) == mapped_owner, "UDP did not cross both real Wintun adapters and relay");
        require(sendto(receiver.get(), reinterpret_cast<const char*>(bytes.data()), received, 0,
                       reinterpret_cast<const sockaddr*>(&remote), remote_size) == received, "Windows UDP echo failed");
        remote_size = sizeof(remote);
        const auto reply = recvfrom(sender.get(), reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                                    reinterpret_cast<sockaddr*>(&remote), &remote_size);
        peer.check();
        if (reply == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) return false;
        require(reply == static_cast<int>(datagram.size()) && std::equal(datagram.begin(), datagram.end(), bytes.begin()) &&
                    ntohl(remote.sin_addr.s_addr) == peer_ip, "UDP reply did not reach the Windows application");
        return true;
    }, "UDP sockets did not recover after peer state convergence");

    auto listener = bound_socket(SOCK_STREAM, mapped_peer);
    require(listen(listener.get(), 1) == 0, "cannot listen on peer virtual address");
    auto client = bound_socket(SOCK_STREAM, owner_ip);
    const auto tcp_destination = endpoint(peer_ip, port_of(listener));
    require(connect(client.get(), reinterpret_cast<const sockaddr*>(&tcp_destination), sizeof(tcp_destination)) == 0,
            "Windows TCP handshake failed across virtual network");
    Socket server(accept(listener.get(), nullptr, nullptr));
    require(server.get() != INVALID_SOCKET, "virtual TCP server did not accept");
    timeouts(server);
    std::vector<std::byte> message(65536);
    for (std::size_t i = 0; i < message.size(); ++i) message[i] = std::byte{static_cast<unsigned char>(i % 251)};
    std::exception_ptr server_error;
    std::jthread server_worker([&] {
        try {
            require(receive_all(server, message.size()) == message, "TCP stream was damaged on the virtual network");
            send_all(server, message);
        } catch (...) { server_error = std::current_exception(); }
    });
    send_all(client, message);
    require(receive_all(client, message.size()) == message, "TCP echo was damaged on the return path");
    server_worker.join();
    if (server_error) std::rethrow_exception(server_error);
    peer.check();
    std::cout << "Real Windows TCP/UDP sockets crossed service, Wintun, encryption and relay in both directions\n" << std::flush;
}

void fanout(Peer& peer, Peer& outsider, std::uint32_t owner_ip) {
    auto sender = bound_socket(SOCK_DGRAM, owner_ip);
    const BOOL enabled = TRUE;
    require(setsockopt(sender.get(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0,
            "cannot enable virtual broadcast");
    in_addr interface_address{};
    interface_address.s_addr = htonl(owner_ip);
    require(setsockopt(sender.get(), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&interface_address),
                       sizeof(interface_address)) == 0, "cannot select virtual multicast interface");
    const std::array<char, 32> payload{'L', 'a', 'n', 'L', 'i', 'n', 'k'};
    for (const auto ip : {(owner_ip & 0xffffff00U) | 255U, 0xefff4d01U}) {
        const auto before = peer.fanout();
        const auto target = endpoint(ip, 42642);
        require(sendto(sender.get(), payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == static_cast<int>(payload.size()),
                "virtual discovery send failed");
        wait_for([&] { peer.check(); return peer.fanout() > before; }, "broadcast or multicast did not reach the approved peer");
        require(peer.fanout() == before + 1, "virtual discovery packet was duplicated");
    }
    require(outsider.deliveries() == 0, "virtual network leaked packets to an unrelated device");
}

void scenario(const std::filesystem::path& root, std::uint16_t port, bool tcp, std::uint16_t relay_port) {
    std::unique_ptr<TcpOnlyProxy> proxy;
    if (tcp) proxy = std::make_unique<TcpOnlyProxy>(port, relay_port);
    Peer peer(root, "peer", port, true);
    Peer outsider(root, "outsider", port);
    wait_for([&] { return diagnostics().authenticated && peer.client.authenticated() && outsider.client.authenticated(); },
             "real Windows service and two peers failed to authenticate");
    const auto created = protocol::decode_network_operation_result(local(protocol::LocalCommand::create,
        protocol::encode_network_create_request({"Windows Wintun E2E"})));
    require(created.code == protocol::NetworkResultCode::success, "service network creation failed");
    const auto id = created.network_id;
    require(peer.client.join_network(id).code == protocol::NetworkResultCode::pending_approval,
            "unapproved peer bypassed membership approval");
    wait_for([&] {
        const auto activity = protocol::decode_local_activity(local(protocol::LocalCommand::activity));
        return std::any_of(activity.requests.begin(), activity.requests.end(), [&](const auto& request) {
            return request.network_id == id && request.device_id == peer.identity.device_id();
        });
    }, "desktop control API did not expose the real join request");
    const auto approved = protocol::decode_network_operation_result(local(protocol::LocalCommand::approve,
        protocol::encode_network_member_request({id, peer.identity.device_id()})));
    require(approved.code == protocol::NetworkResultCode::success, "service peer approval failed");
    require(outsider.client.create_network("Separate Windows LAN").code == protocol::NetworkResultCode::success,
            "outsider network creation failed");
    ready(peer, id, tcp);
    const auto desktop_peers = protocol::decode_network_peer_state(local(protocol::LocalCommand::peers,
        protocol::encode_network_selection_request({id})));
    require(desktop_peers.peers.size() == 1 && desktop_peers.peers.front().device_id == peer.identity.device_id(),
        "desktop control API did not expose the real virtual device");
    const auto original = *peer.client.cached_peer_state(id);
    const auto owner_ip = original.peers.front().ipv4_address;
    const auto peer_ip = original.own_address;
    peer.attach(id);
    application_roundtrip(peer, owner_ip, peer_ip);
    fanout(peer, outsider, owner_ip);

    command(root, 1, "service-stop");
    wait_for([&] { return !address_luid(owner_ip); }, "service stop did not remove its virtual address");
    command(root, 2, "service-start");
    ready(peer, id, tcp ? std::optional<bool>{true} : std::nullopt);
    require(peer.client.cached_peer_state(id)->owner_device_id == original.owner_device_id &&
                peer.client.cached_peer_state(id)->peers.front().ipv4_address == owner_ip,
            "service restart changed the persisted device identity or virtual lease");
    application_roundtrip(peer, owner_ip, peer_ip);

    command(root, 3, "relay-stop");
    wait_for([&] { return !peer.client.authenticated() && !diagnostics().authenticated && !address_luid(owner_ip); },
             "relay outage did not revoke authentication and Windows routes");
    command(root, 4, "relay-start");
    ready(peer, id, tcp ? std::optional<bool>{true} : std::nullopt);
    require(peer.client.cached_peer_state(id)->own_address == peer_ip &&
                peer.client.cached_peer_state(id)->peers.front().ipv4_address == owner_ip,
            "relay restart lost persisted virtual leases");
    application_roundtrip(peer, owner_ip, peer_ip);
    command(root, 5, "upgrade-failure");
    ready(peer, id, tcp ? std::optional<bool>{true} : std::nullopt);
    application_roundtrip(peer, owner_ip, peer_ip);
    command(root, 6, "upgrade-failure");
    ready(peer, id, tcp ? std::optional<bool>{true} : std::nullopt);
    application_roundtrip(peer, owner_ip, peer_ip);
    command(root, 7, "upgrade-locked-config");
    ready(peer, id, tcp ? std::optional<bool>{true} : std::nullopt);
    application_roundtrip(peer, owner_ip, peer_ip);
    command(root, 8, "upgrade-locked-token");
    ready(peer, id, tcp ? std::optional<bool>{true} : std::nullopt);
    application_roundtrip(peer, owner_ip, peer_ip);
    require(outsider.deliveries() == 0, "traffic escaped its virtual network after restart");
    const auto kicked = protocol::decode_network_operation_result(local(protocol::LocalCommand::kick,
        protocol::encode_network_member_request({id, peer.identity.device_id()})));
    require(kicked.code == protocol::NetworkResultCode::success, "service could not revoke peer membership");
    wait_for([&] { return !peer.client.cached_peer_state(id) && !peer.client.cached_network_key(id); },
             "kicked peer retained network state");
    std::cout << (tcp ? "TLS fallback" : "QUIC")
              << ": real service, two Wintun adapters, TCP/UDP, discovery, isolation, restarts and running-upgrade rollback passed\n";
}

void tls_probe(const std::string& host, std::uint16_t port, bool expected) {
    bool accepted = false;
    std::string failure;
    try {
        std::atomic_bool stopping = false;
        auto channel = transport::TlsChannel::connect(host, port, 3s, stopping);
        channel->stop();
        accepted = true;
    } catch (const std::exception& error) { failure = error.what(); }
    require(accepted == expected, "TLS trust or hostname verification returned the wrong result");
    if (!expected) require(failure == "TLS relay handshake failed", "TLS rejection did not reach certificate validation: " + failure);
    std::cout << "Windows TLS trust probe: " << host << " " << (accepted ? "accepted" : "rejected") << '\n';
}

}

int main(int argc, char* argv[]) {
    try {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "cannot initialize Windows sockets");
        if (argc == 5 && std::string(argv[1]) == "--tls-probe") {
            tls_probe(argv[2], static_cast<std::uint16_t>(std::stoul(argv[3])), std::string(argv[4]) == "accept");
            return 0;
        }
        require(argc == 5, "usage: windows-network-tests fixture port quic|tcp relay-port");
        scenario(argv[1], static_cast<std::uint16_t>(std::stoul(argv[2])), std::string(argv[3]) == "tcp",
                 static_cast<std::uint16_t>(std::stoul(argv[4])));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows network E2E: " << error.what() << '\n';
        return 1;
    }
}
