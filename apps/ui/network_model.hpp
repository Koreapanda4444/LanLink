#pragma once

#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/user_access.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/protocol/path_diagnostics.hpp"

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lanlink::ui {

struct NetworkSnapshot {
    std::vector<protocol::NetworkSummary> networks;
    std::string message;
    std::string error;
    bool service_online = false;
    bool busy = false;
    std::optional<protocol::LocalDiagnostics> diagnostics;
    std::uint64_t diagnostics_generation = 0;
    std::optional<protocol::NetworkPeerState> peers;
    std::optional<protocol::PathDiagnostics> paths;
    std::optional<protocol::GameDiagnostics> games;
    protocol::LocalActivity activity;
    std::optional<protocol::AccessResult> access;
    std::uint64_t access_generation = 0;
    std::optional<protocol::NetworkOperationResult> last_operation;
    std::uint64_t operation_generation = 0;
    std::chrono::steady_clock::time_point peers_updated_at{},paths_updated_at{};
};

class NetworkModel {
public:
    NetworkModel();
    ~NetworkModel();

    NetworkModel(const NetworkModel&) = delete;
    NetworkModel& operator=(const NetworkModel&) = delete;

    void submit(protocol::LocalCommand command, std::vector<std::byte> payload = {});
    [[nodiscard]] NetworkSnapshot snapshot() const;

private:
    void process();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<protocol::LocalMessage> pending_;
    NetworkSnapshot state_;
    bool paths_supported_=true;
    bool games_supported_ = true;
    bool stopping_ = false;
    std::thread worker_;
};

}
