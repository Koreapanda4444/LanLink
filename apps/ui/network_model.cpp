#include "network_model.hpp"

#include "lanlink/transport/local_pipe.hpp"

#include <stdexcept>
#include <utility>

namespace lanlink::ui {

NetworkModel::NetworkModel() {
    pending_.push_back({protocol::LocalCommand::list, false, false,
                        protocol::encode_network_list_request()});
    state_.busy = true;
    worker_ = std::thread([this] { process(); });
}

NetworkModel::~NetworkModel() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.clear();
    }
    wake_.notify_one();
    worker_.join();
}

void NetworkModel::submit(const protocol::LocalCommand command,
                          std::vector<std::byte> payload) {
    if ((command < protocol::LocalCommand::list ||
         command > protocol::LocalCommand::kick) &&
        command != protocol::LocalCommand::diagnostics &&
        command != protocol::LocalCommand::peers && command != protocol::LocalCommand::activity) {
        throw std::invalid_argument("unsupported desktop network command");
    }
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    if (command == protocol::LocalCommand::list ||
        command == protocol::LocalCommand::diagnostics || command == protocol::LocalCommand::peers ||
        command == protocol::LocalCommand::activity) {
        for (const auto& request : pending_) {
            if (request.command == command) return;
        }
    }
    pending_.push_back({command, false, false, std::move(payload)});
    state_.busy = true;
    wake_.notify_one();
}

NetworkSnapshot NetworkModel::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void NetworkModel::process() {
    for (;;) {
        protocol::LocalMessage request;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            request = std::move(pending_.front());
            pending_.pop_front();
        }

        try {
            const auto response = transport::local_pipe_request(request);
            if (request.command == protocol::LocalCommand::list) {
                auto networks = protocol::decode_network_list_result(response.payload);
                std::lock_guard lock(mutex_);
                state_.networks = std::move(networks.networks);
                state_.service_online = true;
                state_.error.clear();
            } else if (request.command == protocol::LocalCommand::diagnostics) {
                auto diagnostics = protocol::decode_local_diagnostics(response.payload);
                std::lock_guard lock(mutex_);
                state_.diagnostics = std::move(diagnostics);
                ++state_.diagnostics_generation;
                state_.service_online = true;
                state_.error.clear();
            } else if (request.command == protocol::LocalCommand::peers) {
                auto peers = protocol::decode_network_peer_state(response.payload);
                std::lock_guard lock(mutex_);
                state_.peers = std::move(peers);
            } else if (request.command == protocol::LocalCommand::activity) {
                auto activity = protocol::decode_local_activity(response.payload);
                std::lock_guard lock(mutex_);
                state_.activity = std::move(activity);
            } else {
                const auto result = protocol::decode_network_operation_result(response.payload);
                std::lock_guard lock(mutex_);
                const auto successful = result.code == protocol::NetworkResultCode::success ||
                                        result.code == protocol::NetworkResultCode::pending_approval;
                if (successful) {
                    state_.message = result.code == protocol::NetworkResultCode::pending_approval
                        ? "Join request sent. Waiting for the network owner to approve this device."
                        : "Network updated.";
                    state_.error.clear();
                } else {
                    state_.message.clear();
                    state_.error = std::string{protocol::network_result_code_name(result.code)};
                }
                if (result.code == protocol::NetworkResultCode::success ||
                    result.code == protocol::NetworkResultCode::pending_approval) {
                    pending_.push_back({protocol::LocalCommand::list, false, false,
                                        protocol::encode_network_list_request()});
                    wake_.notify_one();
                }
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            if (request.command != protocol::LocalCommand::peers && request.command != protocol::LocalCommand::activity) {
                state_.error = error.what();
                state_.message.clear();
            }
            if (request.command == protocol::LocalCommand::peers) state_.peers.reset();
            if (request.command == protocol::LocalCommand::list) state_.networks.clear();
            if (request.command == protocol::LocalCommand::diagnostics ||
                (request.command == protocol::LocalCommand::list && !state_.diagnostics)) {
                state_.service_online = false;
                state_.diagnostics.reset();
                state_.peers.reset();
                state_.activity = {};
            }
        }

        {
            std::lock_guard lock(mutex_);
            state_.busy = !pending_.empty();
        }
    }
}

}
