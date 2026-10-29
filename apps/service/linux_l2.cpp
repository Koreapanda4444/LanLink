#include "lanlink/core/config.hpp"
#include "lanlink/auth/directory.hpp"
#include "lanlink/transport/directory_fetch.hpp"
#include "lanlink/network/linux_tap.hpp"
#include "lanlink/network/virtual_network.hpp"
#include "lanlink/session/peer_packet_path.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
lanlink::protocol::NetworkId parse_network(std::string_view text) {
    lanlink::protocol::NetworkId id{};
    if (text.size() != id.size() * 2) throw std::invalid_argument("invalid network ID");
    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw std::invalid_argument("invalid network ID");
    };
    for (std::size_t i = 0; i < id.size(); ++i)
        id[i] = std::byte{static_cast<unsigned char>((digit(text[i * 2]) << 4U) | digit(text[i * 2 + 1]))};
    return id;
}
}
int main(int argc, char* argv[]) {
    try {
        if (argc != 3) throw std::invalid_argument("usage: lanlink-l2 <config-path> <network-id>");
        const auto config = lanlink::core::load_config(argv[1]);
        lanlink::core::validate_service_config(config);
        const auto id = parse_network(argv[2]);
        lanlink::core::Logger logger(config.log_directory / "l2.log", "linux-l2",
            config.log_level, config.log_max_size_bytes, config.log_max_files);
        lanlink::transport::QuicClientOptions options;
        const auto bootstrap=lanlink::auth::resolve_bootstrap(config,lanlink::transport::fetch_directory,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
        if(!bootstrap.error.empty()) logger.warning(bootstrap.error);
        options.relay_host=bootstrap.relay.host; options.port=bootstrap.relay.port;
        options.identity_file = config.device_identity_file; options.auth_token_file = config.auth_token_file;
        options.handshake_timeout = std::chrono::milliseconds{config.quic_handshake_timeout_ms};
        options.authentication_timeout = std::chrono::milliseconds{config.authentication_timeout_ms};
        options.idle_timeout = std::chrono::milliseconds{config.quic_idle_timeout_ms};
        options.keep_alive_interval_ms = config.quic_keep_alive_interval_ms;
        options.reconnect_initial_delay = std::chrono::milliseconds{config.reconnect_initial_delay_ms};
        options.reconnect_maximum_delay = std::chrono::milliseconds{config.reconnect_max_delay_ms};
        lanlink::session::RelaySession session(std::move(options), logger);
        lanlink::session::PeerPathOptions peer_options;
        peer_options.enabled = config.p2p_enabled;
        if (!config.p2p_stun_host.empty()) {
            in_addr address{};
            if (inet_pton(AF_INET, config.p2p_stun_host.c_str(), &address) != 1)
                throw std::invalid_argument("STUN host must be an IPv4 address");
            peer_options.stun_servers.push_back({ntohl(address.s_addr), config.p2p_stun_port});
        }
        lanlink::session::PeerPacketPath path(session, config.device_identity_file, logger, std::move(peer_options));
        auto adapter = std::make_shared<lanlink::network::LinuxTapAdapter>();
        lanlink::network::VirtualNetwork network(adapter, path, logger);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        network.start();
        std::cout << "Experimental isolated Ethernet mode on " << adapter->name()
                  << ". All participating devices must use this mode.\n" << std::flush;
        bool joined = false;
        while (!stopped && !session.finished()) {
            if (!session.client().authenticated()) joined = false;
            else if (!joined) {
                const auto result = session.client().join_network(id);
                if (result.code != lanlink::protocol::NetworkResultCode::success &&
                    result.code != lanlink::protocol::NetworkResultCode::pending_approval)
                    throw std::runtime_error(std::string(lanlink::protocol::network_result_code_name(result.code)));
                joined = true;
            }
            auto snapshot = session.snapshot();
            std::erase_if(snapshot.networks, [&](const auto& item) { return item.peers.network_id != id; });
            network.synchronize(snapshot);
            if (const auto error = network.pump_error()) std::rethrow_exception(error);
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        network.stop();
        session.rethrow_error();
        return stopped ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-l2: " << error.what() << '\n';
        return 1;
    }
}
