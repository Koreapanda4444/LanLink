#include "windows_service.hpp"

#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/transport/local_pipe.hpp"
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
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <optional>
#include <string_view>
#include <vector>

namespace {

volatile std::sig_atomic_t console_stop_requested = 0;

void handle_signal(int) {
    console_stop_requested = 1;
}

void run_runtime(const std::optional<std::filesystem::path>& config_path,
                 const std::function<void()>& ready) {
    using lanlink::core::Component;

    auto config = config_path
                      ? lanlink::core::load_config(*config_path)
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
    std::atomic_bool worker_finished = false;
    std::exception_ptr worker_error;
    std::jthread worker;
    try {
        worker = std::jthread([&](const std::stop_token token) {
            try {
                client.run(token);
            } catch (...) {
                worker_error = std::current_exception();
            }
            worker_finished.store(true);
        });
    } catch (...) {
        adapter->stop();
        client.set_encrypted_packet_handler({});
        throw;
    }

    std::map<lanlink::protocol::NetworkId,
             lanlink::transport::WintunNetworkAddress> installed;
    std::exception_ptr adapter_error;

    std::unique_ptr<lanlink::transport::LocalPipeServer> pipe;
    try {
        pipe = std::make_unique<lanlink::transport::LocalPipeServer>([&](
            const lanlink::protocol::LocalMessage& request) {
            namespace protocol = lanlink::protocol;
            std::vector<std::byte> payload;
            switch (request.command) {
            case protocol::LocalCommand::status: {
                if (!request.payload.empty()) {
                    throw std::invalid_argument("status does not accept a payload");
                }
                const auto count = client.cached_peer_states().size();
                payload = {std::byte{static_cast<unsigned char>(client.connected())},
                           std::byte{static_cast<unsigned char>(client.authenticated())},
                           std::byte{static_cast<unsigned char>(client.using_tcp_fallback())},
                           std::byte{0}};
                for (std::size_t i = 0; i < 4; ++i) {
                    payload.push_back(static_cast<std::byte>((count >> (i * 8)) & 0xff));
                }
                break;
            }
            case protocol::LocalCommand::list: {
                static_cast<void>(protocol::decode_network_list_request(request.payload));
                const auto result = client.list_networks();
                if (result.code != protocol::NetworkResultCode::success) {
                    throw std::runtime_error(std::string{
                        protocol::network_result_code_name(result.code)});
                }
                payload = protocol::encode_network_list_result(result.result);
                break;
            }
            case protocol::LocalCommand::create: {
                const auto name = protocol::decode_network_create_request(request.payload).name;
                payload = protocol::encode_network_operation_result(
                    client.create_network(name));
                break;
            }
            case protocol::LocalCommand::join:
            case protocol::LocalCommand::leave: {
                const auto id = protocol::decode_network_selection_request(
                    request.payload).network_id;
                payload = protocol::encode_network_operation_result(
                    request.command == protocol::LocalCommand::join
                        ? client.join_network(id) : client.leave_network(id));
                break;
            }
            case protocol::LocalCommand::invite:
            case protocol::LocalCommand::approve:
            case protocol::LocalCommand::kick: {
                const auto member = protocol::decode_network_member_request(request.payload);
                protocol::NetworkOperationResult result;
                if (request.command == protocol::LocalCommand::invite) {
                    result = client.invite_member(member.network_id, member.device_id);
                } else if (request.command == protocol::LocalCommand::approve) {
                    result = client.approve_member(member.network_id, member.device_id);
                } else {
                    result = client.kick_member(member.network_id, member.device_id);
                }
                payload = protocol::encode_network_operation_result(result);
                break;
            }
            case protocol::LocalCommand::stop:
                if (!request.payload.empty()) {
                    throw std::invalid_argument("stop does not accept a payload");
                }
                lanlink::service::request_stop();
                break;
            case protocol::LocalCommand::diagnostics: {
                if (!request.payload.empty()) {
                    throw std::invalid_argument("diagnostics does not accept a payload");
                }
                const auto current = client.diagnostics();
                protocol::LocalDiagnostics snapshot;
                snapshot.connected = current.connected;
                snapshot.authenticated = current.authenticated;
                snapshot.tcp_fallback = current.tcp_fallback;
                snapshot.active_networks = static_cast<std::uint32_t>(
                    client.cached_peer_states().size());
                snapshot.rtt_us = current.rtt_us;
                snapshot.sent_bytes = current.sent_bytes;
                snapshot.received_bytes = current.received_bytes;
                snapshot.reconnects = current.reconnects;
                snapshot.last_error = current.last_error;
                payload = protocol::encode_local_diagnostics(snapshot);
                break;
            }
            }
            return protocol::LocalMessage{request.command, true, false,
                                          std::move(payload)};
        });
    } catch (...) {
        adapter->stop();
        bridge->clear();
        client.set_encrypted_packet_handler({});
        worker.request_stop();
        client.stop();
        worker.join();
        throw;
    }

    const auto shutdown = [&] {
        pipe->stop();
        adapter->stop();
        bridge->clear();
        client.set_encrypted_packet_handler({});
        worker.request_stop();
        client.stop();
        worker.join();
    };
    try {
        ready();
        while (console_stop_requested == 0 &&
               !lanlink::service::stop_requested() && !worker_finished.load()) {
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
    } catch (...) {
        shutdown();
        throw;
    }

    shutdown();

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
    if (adapter_error) {
        std::rethrow_exception(adapter_error);
    }
    logger.info("stopped");
    logger.flush();
}

}

int wmain(const int argc, wchar_t* argv[]) {
    try {
        const auto action = argc > 1 ? std::wstring_view{argv[1]} : std::wstring_view{};
        if (action == L"--install" && argc <= 3) {
            const std::optional<std::filesystem::path> path = argc == 3
                ? std::optional<std::filesystem::path>{argv[2]} : std::nullopt;
            if (path) {
                lanlink::core::validate_service_config(lanlink::core::load_config(*path));
            }
            lanlink::service::install(path);
            std::cout << "LanLink service installed for automatic startup\n";
        } else if (action == L"--uninstall" && argc == 2) {
            lanlink::service::uninstall();
            std::cout << "LanLink service uninstalled\n";
        } else if (action == L"--start" && argc == 2) {
            lanlink::service::start();
            std::cout << "LanLink service running\n";
        } else if (action == L"--stop" && argc == 2) {
            lanlink::service::stop();
            std::cout << "LanLink service stopped\n";
        } else if (action == L"--service" && argc == 4) {
            const std::optional<std::filesystem::path> path =
                std::wstring_view{argv[2]} == L"-"
                    ? std::nullopt
                    : std::optional<std::filesystem::path>{argv[2]};
            lanlink::service::dispatch(path, std::filesystem::path{argv[3]},
                [](const auto& config, const auto& ready) {
                    run_runtime(config, ready);
                });
        } else if ((action == L"--console" && argc <= 3) ||
                   (action.empty() && argc == 1) ||
                   (argc == 2 && !action.starts_with(L"--"))) {
            const std::optional<std::filesystem::path> path =
                argc == 1 || (action == L"--console" && argc == 2)
                    ? std::nullopt
                    : std::optional<std::filesystem::path>{
                          action == L"--console" ? argv[2] : argv[1]};
            console_stop_requested = 0;
            std::signal(SIGINT, handle_signal);
            std::signal(SIGTERM, handle_signal);
            run_runtime(path, [] {});
        } else {
            throw std::invalid_argument(
                "usage: lanlink-service [config-path] | --console [config-path] | "
                "--install [config-path] | --start | --stop | --uninstall");
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-service: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
