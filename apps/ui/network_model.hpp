#pragma once

#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/network_messages.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
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
    bool stopping_ = false;
    std::thread worker_;
};

}
