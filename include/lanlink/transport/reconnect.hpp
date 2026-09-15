#pragma once

#include <chrono>

namespace lanlink::transport {

class ReconnectBackoff {
public:
    ReconnectBackoff(std::chrono::milliseconds initial_delay,
                     std::chrono::milliseconds maximum_delay);

    [[nodiscard]] std::chrono::milliseconds next_delay() noexcept;
    void reset() noexcept;
    [[nodiscard]] std::chrono::milliseconds current_delay() const noexcept;

private:
    std::chrono::milliseconds initial_delay_;
    std::chrono::milliseconds maximum_delay_;
    std::chrono::milliseconds current_delay_;
};

}
