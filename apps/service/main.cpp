#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/packet_bridge.hpp"
#include "lanlink/transport/quic.hpp"
#include "lanlink/transport/wintun_adapter.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

}

int main(const int argc, char* argv[]) {
    using lanlink::core::Component;

    try {
        if (argc > 2) {
            throw std::invalid_argument("usage: lanlink-service [config-path]");
        }

        auto config = argc == 2
                          ? lanlink::core::load_config(std::filesystem::path{argv[1]})
                          : lanlink::core::RuntimeConfig{};
        lanlink::core::validate_service_config(config);

        lanlink::core::Logger logger(config.log_directory / "service.log",
                                     "service",
                                     config.log_level,
                                     config.log_max_size_bytes,
                                     config.log_max_files);
        logger.info("starting");
        std::cout << "LanLink " << lanlink::core::component_name(Component::service)
                  << " " << lanlink::core::project_version() << '\n';

        lanlink::transport::QuicClientOptions options;
        options.relay_host = config.relay_host;
        options.port = config.relay_port;
        options.identity_file = config.device_identity_file;
        options.auth_token_file = config.auth_token_file;
        options.handshake_timeout = std::chrono::milliseconds{config.quic_handshake_timeout_ms};
        options.authentication_timeout =
            std::chrono::milliseconds{config.authentication_timeout_ms};
        options.idle_timeout = std::chrono::milliseconds{config.quic_idle_timeout_ms};
        options.keep_alive_interval_ms = config.quic_keep_alive_interval_ms;
        options.reconnect_initial_delay =
            std::chrono::milliseconds{config.reconnect_initial_delay_ms};
        options.reconnect_maximum_delay =
            std::chrono::milliseconds{config.reconnect_max_delay_ms};

        lanlink::transport::QuicRelayClient client(std::move(options), logger);
        auto adapter = std::make_shared<lanlink::transport::WintunAdapter>();
        auto bridge = std::make_shared<lanlink::transport::VirtualPacketBridge>(
            [&](const lanlink::protocol::EncryptedNetworkPacket& packet) {
                client.send_encrypted_packet(packet);
            },
            [adapter](const std::span<const std::byte> packet) {
                adapter->inject(packet);
            });
        auto last_packet_warning_ms = std::make_shared<std::atomic<std::int64_t>>(0);
        const auto report_packet_error = [last_packet_warning_ms, &logger](
            const char* operation, const std::exception& error) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            auto last = last_packet_warning_ms->load();
            if (now - last >= 5000 &&
                last_packet_warning_ms->compare_exchange_strong(last, now)) {
                logger.warning(std::string{operation} + ": " + error.what());
            }
        };
        client.set_encrypted_packet_handler(
            [weak = std::weak_ptr{bridge}, report_packet_error](
                const lanlink::protocol::ForwardedNetworkPacket& delivery) {
                if (const auto current = weak.lock()) {
                    try {
                        current->inbound(delivery);
                    } catch (const std::exception& error) {
                        report_packet_error("incoming virtual packet dropped", error);
                    }
                }
            });
        adapter->start([bridge, report_packet_error](
                           const std::span<const std::byte> packet) {
            try {
                bridge->outbound(packet);
            } catch (const std::exception& error) {
                report_packet_error("outgoing virtual packet dropped", error);
            }
        });
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        std::atomic_bool worker_finished = false;
        std::exception_ptr worker_error;
        std::jthread worker([&](const std::stop_token token) {
            try {
                client.run(token);
            } catch (...) {
                worker_error = std::current_exception();
            }

            worker_finished.store(true);
        });

        std::map<lanlink::protocol::NetworkId,
                 lanlink::transport::WintunNetworkAddress> installed;
        std::exception_ptr adapter_error;

        while (stop_requested == 0 && !worker_finished.load()) {
            try {
                const auto identity = client.cached_packet_identity();
                std::vector<lanlink::transport::ActiveNetworkPacketState> active;
                std::map<lanlink::protocol::NetworkId,
                         lanlink::transport::WintunNetworkAddress> desired;
                if (identity) {
                    for (const auto& state : client.cached_peer_states()) {
                        const auto key = client.cached_network_key(state.network_id);
                        if (!key || key->epoch != state.key_epoch) {
                            continue;
                        }
                        active.push_back({state, *key});
                        desired.emplace(state.network_id,
                            lanlink::transport::WintunNetworkAddress{
                                state.network_id, state.own_address,
                                state.subnet_address, state.prefix_length});
                    }
                }
                for (const auto& [id, address] : desired) {
                    adapter->configure(address);
                    installed.insert_or_assign(id, address);
                }
                bridge->synchronize(identity, active);
                for (auto it = installed.begin(); it != installed.end();) {
                    if (!desired.contains(it->first)) {
                        adapter->remove(it->first);
                        it = installed.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (const auto error = adapter->pump_error()) {
                    adapter_error = error;
                    break;
                }
            } catch (const std::exception& error) {
                report_packet_error("virtual adapter synchronization failed", error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }

        adapter->stop();
        bridge->clear();
        client.set_encrypted_packet_handler({});
        worker.request_stop();
        client.stop();
        worker.join();

        if (worker_error) {
            std::rethrow_exception(worker_error);
        }
        if (adapter_error) {
            std::rethrow_exception(adapter_error);
        }
        logger.info("stopped");
        logger.flush();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-service: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
