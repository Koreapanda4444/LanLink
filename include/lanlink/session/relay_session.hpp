#pragma once

#include "lanlink/session/network_state.hpp"
#include "lanlink/transport/quic.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

namespace lanlink::session {

class RelaySession {
public:
    RelaySession(transport::QuicClientOptions options, core::Logger& logger);
    ~RelaySession();
    RelaySession(const RelaySession&) = delete;
    RelaySession& operator=(const RelaySession&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool finished() const noexcept;
    void rethrow_error() const;
    [[nodiscard]] NetworkSnapshot snapshot() const;
    [[nodiscard]] transport::QuicRelayClient& client() noexcept;

private:
    transport::QuicRelayClient client_;
    std::atomic_bool finished_ = true;
    mutable std::mutex error_mutex_;
    std::exception_ptr error_;
    std::jthread worker_;
};

}
