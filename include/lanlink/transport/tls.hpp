#pragma once

#include "lanlink/protocol/message.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace lanlink::transport {

struct TlsTrafficCounters {
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
};

class TlsChannel {
public:
    using FrameHandler = std::function<void(const protocol::Frame&)>;

    static std::shared_ptr<TlsChannel> connect(
        const std::string& host, std::uint16_t port,
        std::chrono::milliseconds timeout, const std::atomic_bool& stopping);

    ~TlsChannel();
    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    bool enqueue(const protocol::Frame& frame) noexcept;
    [[nodiscard]] bool pending_output() const noexcept;
    void run(const FrameHandler& on_frame, const std::function<bool()>& keep_running);
    void stop() noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] TlsTrafficCounters traffic() const noexcept;

private:
    class Impl;
    explicit TlsChannel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class TlsListener;
};

class TlsListener {
public:
    struct Handler {
        TlsChannel::FrameHandler on_frame;
        std::function<bool()> keep_running;
        std::function<void()> on_close;
    };
    using HandlerFactory = std::function<Handler(std::shared_ptr<TlsChannel>)>;

    TlsListener(std::string host, std::uint16_t port,
                std::filesystem::path certificate, std::filesystem::path private_key,
                std::chrono::milliseconds handshake_timeout, HandlerFactory factory);
    ~TlsListener();
    TlsListener(const TlsListener&) = delete;
    TlsListener& operator=(const TlsListener&) = delete;

    void start();
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
