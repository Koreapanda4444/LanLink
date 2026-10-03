#include "lanlink/transport/resource_limits.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace lanlink::transport {

struct SendBudget::State {
    explicit State(SendQueueLimits value) : limits(value) {}
    SendQueueLimits limits;
    std::mutex mutex;
    std::size_t bytes = 0;
    std::size_t frames = 0;
    std::uint64_t rejected = 0;
};

SendBudget::SendBudget(SendQueueLimits limits) : state_(std::make_shared<State>(limits)) {
    if (limits.bytes == 0 || limits.frames == 0 ||
        limits.reserved_control_bytes >= limits.bytes ||
        limits.reserved_control_frames >= limits.frames) {
        throw std::invalid_argument("invalid send queue limits");
    }
}

std::optional<SendBudget::Ticket> SendBudget::reserve(std::size_t size, bool control) {
    std::lock_guard lock(state_->mutex);
    const auto& limits = state_->limits;
    const auto byte_limit = limits.bytes - (control ? 0 : limits.reserved_control_bytes);
    const auto frame_limit = limits.frames - (control ? 0 : limits.reserved_control_frames);
    if (size == 0 || size > byte_limit || state_->bytes > byte_limit - size ||
        state_->frames >= frame_limit) {
        ++state_->rejected;
        return std::nullopt;
    }
    state_->bytes += size;
    ++state_->frames;
    return Ticket(state_, size);
}

SendBudget::Usage SendBudget::usage() const noexcept {
    std::lock_guard lock(state_->mutex);
    return {state_->bytes, state_->frames, state_->rejected};
}

SendBudget::Ticket::Ticket(std::shared_ptr<State> state, std::size_t size) noexcept
    : state_(std::move(state)), size_(size) {}
SendBudget::Ticket::~Ticket() { release(); }
SendBudget::Ticket::Ticket(Ticket&& other) noexcept
    : state_(std::move(other.state_)), size_(other.size_) {}
SendBudget::Ticket& SendBudget::Ticket::operator=(Ticket&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        size_ = other.size_;
    }
    return *this;
}
void SendBudget::Ticket::release() noexcept {
    if (state_) {
        {
            std::lock_guard lock(state_->mutex);
            state_->bytes -= size_;
            --state_->frames;
        }
        state_.reset();
    }
}

ControlRateLimit::ControlRateLimit(std::uint32_t requests_per_second)
    : rate_(requests_per_second), capacity_(4.0 * rate_), tokens_(capacity_) {
    if (requests_per_second == 0) {
        throw std::invalid_argument("control request rate must be positive");
    }
}

bool ControlRateLimit::allow(Clock::time_point now) noexcept {
    if (updated_ && now > *updated_) {
        tokens_ = std::min(capacity_, tokens_ +
            std::chrono::duration<double>(now - *updated_).count() * rate_);
    }
    if (!updated_ || now > *updated_) {
        updated_ = now;
    }
    if (tokens_ < 1.0) {
        return false;
    }
    tokens_ -= 1.0;
    return true;
}

}
