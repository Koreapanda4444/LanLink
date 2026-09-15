#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace lanlink::core {
class Logger;
}

namespace lanlink::transport {

struct QuicServerOptions {
    std::string listen_host = "0.0.0.0";
    std::uint16_t port = 4433;
    std::filesystem::path certificate_file;
    std::filesystem::path private_key_file;
    std::filesystem::path auth_token_file;
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::uint32_t keep_alive_interval_ms = 15'000;
};

struct QuicClientOptions {
    std::string relay_host = "127.0.0.1";
    std::uint16_t port = 4433;
    std::filesystem::path identity_file;
    std::filesystem::path auth_token_file;
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::uint32_t keep_alive_interval_ms = 15'000;
    std::chrono::milliseconds reconnect_initial_delay{1'000};
    std::chrono::milliseconds reconnect_maximum_delay{30'000};
};

class QuicRelayServer {
public:
    QuicRelayServer(QuicServerOptions options, core::Logger& logger);
    ~QuicRelayServer();

    QuicRelayServer(const QuicRelayServer&) = delete;
    QuicRelayServer& operator=(const QuicRelayServer&) = delete;
    QuicRelayServer(QuicRelayServer&&) = delete;
    QuicRelayServer& operator=(QuicRelayServer&&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class QuicRelayClient {
public:
    QuicRelayClient(QuicClientOptions options, core::Logger& logger);
    ~QuicRelayClient();

    QuicRelayClient(const QuicRelayClient&) = delete;
    QuicRelayClient& operator=(const QuicRelayClient&) = delete;
    QuicRelayClient(QuicRelayClient&&) = delete;
    QuicRelayClient& operator=(QuicRelayClient&&) = delete;

    void run(std::stop_token stop_token);
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool authenticated() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
