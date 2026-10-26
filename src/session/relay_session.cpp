#include "lanlink/session/relay_session.hpp"

#include <stdexcept>
#include <utility>

namespace lanlink::session {

RelaySession::RelaySession(transport::QuicClientOptions options, core::Logger& logger)
    : client_(std::move(options), logger) {}

RelaySession::~RelaySession() { stop(); }

void RelaySession::start() {
    if (worker_.joinable()) throw std::logic_error("relay session is already started");
    {
        std::lock_guard lock(error_mutex_);
        error_ = {};
    }
    finished_.store(false);
    try {
        worker_ = std::jthread([this](std::stop_token token) {
            try { client_.run(token); }
            catch (...) {
                std::lock_guard lock(error_mutex_);
                error_ = std::current_exception();
            }
            finished_.store(true);
        });
    } catch (...) {
        finished_.store(true);
        throw;
    }
}

void RelaySession::stop() noexcept {
    worker_.request_stop();
    client_.stop();
    if (worker_.joinable()) worker_.join();
}

bool RelaySession::finished() const noexcept { return finished_.load(); }

void RelaySession::rethrow_error() const {
    std::lock_guard lock(error_mutex_);
    if (error_) std::rethrow_exception(error_);
}

NetworkSnapshot RelaySession::snapshot() const {
    NetworkSnapshot result;
    if (!client_.authenticated()) return result;
    result.identity = client_.cached_packet_identity();
    if (result.identity) {
        for (const auto& peers : client_.cached_peer_states()) {
            const auto key = client_.cached_network_key(peers.network_id);
            if (key && key->epoch == peers.key_epoch) result.networks.push_back({peers, *key});
        }
    }
    if (!client_.authenticated()) return {};
    return result;
}

transport::QuicRelayClient& RelaySession::client() noexcept { return client_; }

}
