#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lanlink::transport {

struct SendQueueLimits {
    std::size_t bytes = 4U * 1024U * 1024U;
    std::size_t frames = 256;
    std::size_t reserved_control_bytes = 64U * 1024U;
    std::size_t reserved_control_frames = 8;
};

class SendBudget {
    struct State;

public:
    struct Usage {
        std::size_t bytes;
        std::size_t frames;
        std::uint64_t rejected;
    };

    class Ticket {
    public:
        ~Ticket();
        Ticket(Ticket&& other) noexcept;
        Ticket& operator=(Ticket&& other) noexcept;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;

    private:
        Ticket(std::shared_ptr<State> state, std::size_t size) noexcept;
        void release() noexcept;
        std::shared_ptr<State> state_;
        std::size_t size_;
        friend class SendBudget;
    };

    explicit SendBudget(SendQueueLimits limits = {});
    [[nodiscard]] std::optional<Ticket> reserve(std::size_t size, bool control);
    [[nodiscard]] Usage usage() const noexcept;

private:
    std::shared_ptr<State> state_;
};

class ControlRateLimit {
public:
    using Clock = std::chrono::steady_clock;
    explicit ControlRateLimit(std::uint32_t requests_per_second = 32);
    [[nodiscard]] bool allow(Clock::time_point now = Clock::now()) noexcept;

private:
    double rate_;
    double capacity_;
    double tokens_;
    std::optional<Clock::time_point> updated_;
};

}
