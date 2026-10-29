#include "../apps/ui/network_model.hpp"

#include "lanlink/transport/local_pipe.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

template <typename Predicate>
void wait_until(const char* operation, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error(std::string{operation} + ": desktop network update timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

}

int main() {
    try {
        namespace protocol = lanlink::protocol;
        protocol::NetworkSummary network;
        network.network_id[0] = std::byte{1};
        network.owner_device_id[0] = std::byte{2};
        network.name = "test-room";
        network.member_count = 1;
        std::atomic<bool> relay_unavailable{false},paths_available{true},games_available{true};
        lanlink::transport::LocalPipeServer server([&](
            const protocol::LocalMessage& request) {
            if (request.command == protocol::LocalCommand::list) {
                if (relay_unavailable) throw std::runtime_error("relay unavailable");
                return protocol::LocalMessage{request.command, true, false,
                    protocol::encode_network_list_result({{network}})};
            }
            if (request.command == protocol::LocalCommand::diagnostics) {
                protocol::LocalDiagnostics metrics;
                metrics.connected = !relay_unavailable;
                metrics.authenticated = !relay_unavailable;
                metrics.active_networks = 1;
                metrics.sent_bytes = 4096;
                metrics.received_bytes = 8192;
                metrics.rtt_us = 1200;
                return protocol::LocalMessage{request.command, true, false,
                    protocol::encode_local_diagnostics(metrics)};
            }
            if(request.command==protocol::LocalCommand::path_diagnostics) {
                if(!paths_available)throw std::runtime_error("unsupported local command");
                protocol::PathDiagnostics metrics;protocol::PathStatus path;path.kind=protocol::PathKind::direct_quic;
                path.lifecycle=protocol::PathLifecycle::authenticated;path.reason=protocol::PathReason::direct_ready;path.rtt_us=900;
                metrics.peers.push_back({network.network_id,{std::byte{3}},0x0a4d0002,path});
                return protocol::LocalMessage{request.command,true,false,protocol::encode_path_diagnostics(metrics)};
            }
            if (request.command == protocol::LocalCommand::game_diagnostics) {
                if (!games_available) throw std::runtime_error("unsupported local command");
                protocol::GameDiagnostics metrics;
                metrics.networks.push_back({network.network_id, 0x0a4d0001U, 0x0a4d0000U, 24, 1, 7, 8});
                return protocol::LocalMessage{request.command, true, false, protocol::encode_game_diagnostics(metrics)};
            }
            if(request.command==protocol::LocalCommand::user_access) {
                const auto access=protocol::decode_access_request(request.payload);
                protocol::AccessResult result;result.action=access.action;result.network_id=network.network_id;
                return protocol::LocalMessage{request.command,true,false,protocol::encode_access_result(result)};
            }
            if (request.command == protocol::LocalCommand::create) {
                const auto name = protocol::decode_network_create_request(request.payload).name;
                return protocol::LocalMessage{request.command, true, false,
                    protocol::encode_network_operation_result(
                        {protocol::NetworkOperation::create,
                         name == "new-room" ? protocol::NetworkResultCode::success : protocol::NetworkResultCode::permission_denied,
                         name == "new-room" ? network.network_id : protocol::NetworkId{}})};
            }
            if (request.command == protocol::LocalCommand::activity) {
                protocol::LocalActivity activity;
                activity.device_id[0] = std::byte{2};
                activity.requests.push_back({protocol::NetworkEventKind::join_requested, network.network_id, {std::byte{3}}});
                return protocol::LocalMessage{request.command, true, false, protocol::encode_local_activity(activity)};
            }
            if (request.command == protocol::LocalCommand::peers) {
                const auto id = protocol::decode_network_selection_request(request.payload).network_id;
                return protocol::LocalMessage{request.command, true, false, protocol::encode_network_peer_state(
                    {id, 1, 0x0a4d0000U, 24, 0x0a4d0001U, {}, network.owner_device_id, 1})};
            }
            throw std::runtime_error("unexpected command");
        });
        lanlink::ui::NetworkModel model;
        wait_until("initial list", [&] {
            const auto state = model.snapshot();
            return state.service_online && !state.busy &&
                   state.networks.size() == 1 && state.networks[0].name == "test-room";
        });
        model.submit(protocol::LocalCommand::create,
                     protocol::encode_network_create_request({"new-room"}));
        wait_until("create and refresh", [&] {
            const auto state = model.snapshot();
            return !state.busy && state.message == "Network updated." &&
                   state.networks.size() == 1;
        });
        protocol::AccessRequest protected_create;protected_create.action=protocol::AccessAction::network_create_protected;
        protected_create.text="Protected";protected_create.secret="private password";
        model.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(protected_create));
        wait_until("protected create and selection hint",[&]{const auto state=model.snapshot();return !state.busy && state.access && state.last_operation && state.last_operation->operation==protocol::NetworkOperation::create && state.last_operation->network_id==network.network_id;});
        model.submit(protocol::LocalCommand::diagnostics);
        wait_until("diagnostics", [&] {
            const auto state = model.snapshot();
            return !state.busy && state.diagnostics &&
                   state.diagnostics->sent_bytes == 4096 &&
                   state.diagnostics->rtt_us == 1200;
        });
        model.submit(protocol::LocalCommand::path_diagnostics);
        wait_until("peer path diagnostics",[&]{const auto state=model.snapshot();return !state.busy && state.paths && state.paths->peers.size()==1 && state.paths->peers[0].path.kind==protocol::PathKind::direct_quic && state.paths->peers[0].path.rtt_us==900;});
        paths_available=false;model.submit(protocol::LocalCommand::path_diagnostics);
        wait_until("legacy service fallback",[&]{const auto state=model.snapshot();return !state.busy && !state.paths && state.service_online && state.error.empty() && !state.networks.empty();});
        model.submit(protocol::LocalCommand::game_diagnostics);
        wait_until("game telemetry", [&] {
            const auto state = model.snapshot();
            return !state.busy && state.games && state.games->networks.size() == 1 &&
                state.games->networks.front().discovery_received == 8;
        });
        games_available = false; model.submit(protocol::LocalCommand::game_diagnostics);
        wait_until("legacy game telemetry fallback", [&] {
            const auto state = model.snapshot();
            return !state.busy && !state.games && state.service_online && state.error.empty() && !state.networks.empty();
        });
        model.submit(protocol::LocalCommand::activity);
        model.submit(protocol::LocalCommand::peers, protocol::encode_network_selection_request({network.network_id}));
        wait_until("desktop peers and requests", [&] {
            const auto state = model.snapshot();
            return !state.busy && state.activity.requests.size() == 1 && state.peers && state.peers->own_address == 0x0a4d0001U;
        });
        model.submit(protocol::LocalCommand::create, protocol::encode_network_create_request({"denied"}));
        wait_until("operation denial", [&] {
            const auto state = model.snapshot();
            return !state.busy && state.service_online && state.error == "permission_denied" && state.message.empty();
        });
        relay_unavailable = true;
        model.submit(protocol::LocalCommand::diagnostics);
        model.submit(protocol::LocalCommand::list, protocol::encode_network_list_request());
        wait_until("relay outage preserves local service status", [&] {
            const auto state = model.snapshot();
            return !state.busy && state.service_online && state.diagnostics &&
                   !state.diagnostics->connected && state.networks.empty() &&
                   state.error == "relay unavailable";
        });
        server.stop();
        std::cout << "desktop network control validated\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
