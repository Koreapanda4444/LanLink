#include "lanlink/transport/reconnect.hpp"

#include <algorithm>
#include <stdexcept>

namespace lanlink::transport {

ReconnectBackoff::ReconnectBackoff(const std::chrono::milliseconds initial_delay,
                                   const std::chrono::milliseconds maximum_delay)
    : initial_delay_(initial_delay),
      maximum_delay_(maximum_delay),
      current_delay_(initial_delay) {
    if (initial_delay_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("initial reconnect delay must be positive");
    }

    if (maximum_delay_ < initial_delay_) {
        throw std::invalid_argument("maximum reconnect delay must not be less than initial delay");
    }
}

std::chrono::milliseconds ReconnectBackoff::next_delay() noexcept {
    const auto result = current_delay_;
    const auto remaining = maximum_delay_ - current_delay_;
    current_delay_ += std::min(current_delay_, remaining);
    return result;
}

void ReconnectBackoff::reset() noexcept {
    current_delay_ = initial_delay_;
}

std::chrono::milliseconds ReconnectBackoff::current_delay() const noexcept {
    return current_delay_;
}

}
