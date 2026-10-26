#include "windows_service.hpp"

#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/transport/local_pipe.hpp"
#include "lanlink/transport/packet_bridge.hpp"
#include "lanlink/session/relay_session.hpp"
#include "lanlink/session/relay_packet_path.hpp"
#include "lanlink/transport/wintun_adapter.hpp"

#include <atomic>
#include <algorithm>
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
#include <mutex>
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

void run_runtime_impl(const lanlink::core::RuntimeConfig& config,
                      lanlink::core::Logger& logger,
                      const std::function<void()>& ready) {
    using lanlink::core::Component;

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

    std::mutex requests_mutex;
    std::vector<lanlink::protocol::NetworkEvent> requests;
    lanlink::session::RelaySession session(std::move(options), logger);
    auto& client = session.client();
    lanlink::session::RelayPacketPath path(session);
    client.set_network_event_handler([&](const lanlink::protocol::NetworkEvent& event) {
        namespace protocol = lanlink::protocol;
        std::lock_guard lock(requests_mutex);
        std::erase_if(requests, [&](const auto& previous) {
            return previous.network_id == event.network_id && previous.device_id == event.device_id;
        });
        if (event.kind == protocol::NetworkEventKind::join_requested) {
            if (requests.size() == 128) requests.erase(requests.begin());
            requests.push_back(event);
        }
    });
    auto adapter = std::make_shared<lanlink::transport::WintunAdapter>();
    auto bridge = std::make_shared<lanlink::transport::VirtualPacketBridge>(
        [&](const lanlink::protocol::EncryptedNetworkPacket& packet) {
            if (path.send(packet) != lanlink::transport::PacketSendResult::accepted)
                throw std::runtime_error("packet path could not accept the encrypted packet");
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
    path.set_receive_handler(
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
    try {
        path.start();
    } catch (...) {
        adapter->stop();
        path.set_receive_handler({});
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
                if (result.code == protocol::NetworkResultCode::success) {
                    std::lock_guard lock(requests_mutex);
                    std::erase_if(requests, [&](const auto& event) {
                        return event.network_id == member.network_id && event.device_id == member.device_id;
                    });
                }
                payload = protocol::encode_network_operation_result(result);
                break;
            }
            case protocol::LocalCommand::peers: {
                const auto id = protocol::decode_network_selection_request(request.payload).network_id;
                const auto state = client.cached_peer_state(id);
                if (!state) throw std::runtime_error("Network is not active. Wait for approval or reconnection.");
                payload = protocol::encode_network_peer_state(*state);
                break;
            }
            case protocol::LocalCommand::activity: {
                if (!request.payload.empty()) throw std::invalid_argument("activity does not accept a payload");
                protocol::LocalActivity activity;
                if (const auto identity = client.cached_packet_identity()) activity.device_id = identity->device_id;
                std::lock_guard lock(requests_mutex);
                for (const auto& event : requests) {
                    const auto state = client.cached_peer_state(event.network_id);
                    if (state && state->owner_device_id == activity.device_id) activity.requests.push_back(event);
                }
                payload = protocol::encode_local_activity(activity);
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
        path.set_receive_handler({});
        path.stop();
        throw;
    }

    const auto shutdown = [&] {
        pipe->stop();
        adapter->stop();
        bridge->clear();
        path.set_receive_handler({});
        path.stop();
    };
    try {
        ready();
        while (console_stop_requested == 0 &&
               !lanlink::service::stop_requested() && !session.finished()) {
            try {
                const auto snapshot = session.snapshot();
                std::map<lanlink::protocol::NetworkId,
                         lanlink::transport::WintunNetworkAddress> desired;
                for (const auto& network : snapshot.networks) {
                    const auto& state = network.peers;
                    desired.emplace(state.network_id,
                        lanlink::transport::WintunNetworkAddress{
                            state.network_id, state.own_address,
                            state.subnet_address, state.prefix_length});
                }
                for (const auto& [id, address] : desired) {
                    adapter->configure(address);
                    installed.insert_or_assign(id, address);
                }
                bridge->synchronize(snapshot.identity, snapshot.networks);
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

    session.rethrow_error();
    if (adapter_error) {
        std::rethrow_exception(adapter_error);
    }
    logger.info("stopped");
    logger.flush();
}

void run_runtime(const std::optional<std::filesystem::path>& config_path,
                 const std::function<void()>& ready) {
    const auto config = config_path
        ? lanlink::core::load_config(*config_path)
        : lanlink::core::RuntimeConfig{};
    lanlink::core::validate_service_config(config);
    lanlink::core::Logger logger(config.log_directory / "service.log", "service",
        config.log_level, config.log_max_size_bytes, config.log_max_files);
    logger.info("starting");
    try {
        run_runtime_impl(config, logger, ready);
    } catch (const std::exception& error) {
        logger.error(std::string{"service failed: "} + error.what());
        logger.flush();
        throw;
    }
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
        } else if (action == L"--create-shortcut" && argc == 3) {
            lanlink::service::create_shortcut(argv[2]);
            std::cout << "LanLink desktop shortcut created\n";
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
                "--install [config-path] | --start | --stop | --uninstall | --create-shortcut path");
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-service: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
