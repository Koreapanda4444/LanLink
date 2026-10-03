#include "lanlink/transport/reconnect.hpp"
#include "lanlink/transport/resource_limits.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view name) {
    if (!condition) {
        std::cerr << "failed: " << name << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string_view name) {
    try {
        function();
        expect(false, name);
    } catch (const std::exception&) {
    }
}

void test_reconnect_backoff() {
    using namespace std::chrono_literals;

    lanlink::transport::ReconnectBackoff backoff(100ms, 1s);
    expect(backoff.current_delay() == 100ms, "initial delay");
    expect(backoff.next_delay() == 100ms, "first delay");
    expect(backoff.next_delay() == 200ms, "second delay");
    expect(backoff.next_delay() == 400ms, "third delay");
    expect(backoff.next_delay() == 800ms, "fourth delay");
    expect(backoff.next_delay() == 1s, "capped delay");
    expect(backoff.next_delay() == 1s, "repeated cap");

    backoff.reset();
    expect(backoff.current_delay() == 100ms, "reset delay");

    expect_error([] {
        static_cast<void>(lanlink::transport::ReconnectBackoff(
            std::chrono::milliseconds::zero(), std::chrono::seconds{1}));
    }, "zero initial delay");
    expect_error([] {
        static_cast<void>(lanlink::transport::ReconnectBackoff(
            std::chrono::seconds{2}, std::chrono::seconds{1}));
    }, "maximum below initial delay");
}

void test_send_budget() {
    using lanlink::transport::SendBudget;
    SendBudget budget({100, 4, 20, 1});
    auto first = budget.reserve(40, false);
    auto second = budget.reserve(40, false);
    expect(first && second && !budget.reserve(1, false), "data respects reserved control bytes");
    auto control = budget.reserve(20, true);
    expect(control.has_value(), "control still fits when data byte quota is full");
    expect(!budget.reserve(1, true), "control also respects the absolute memory bound");
    first.reset();
    auto replacement = budget.reserve(10, false);
    expect(replacement.has_value(), "completed send releases capacity");
    second = std::move(replacement);
    expect(budget.usage().bytes == 30 && budget.usage().frames == 2,
           "moving completion ownership releases the replaced reservation exactly once");
    second.reset();
    control.reset();
    expect(budget.usage().bytes == 0 && budget.usage().frames == 0,
           "cancelled and completed sends leave no reservations");
    expect(!budget.reserve(static_cast<std::size_t>(-1), true), "oversized reservation cannot wrap");

    std::vector<SendBudget::Ticket> tickets;
    for (int i = 0; i < 3; ++i) {
        tickets.push_back(std::move(*budget.reserve(1, false)));
    }
    expect(!budget.reserve(1, false) && budget.reserve(1, true).has_value(),
           "tiny data frames cannot consume the reserved control slot");
    tickets.clear();

    std::optional<SendBudget::Ticket> retained;
    {
        SendBudget temporary;
        retained = temporary.reserve(100, true);
    }
    retained.reset();
    expect_error([] { SendBudget invalid({10, 1, 10, 0}); }, "invalid byte reservation");
    expect_error([] { SendBudget invalid({10, 1, 0, 1}); }, "invalid frame reservation");
}

void test_concurrent_budget() {
    lanlink::transport::SendBudget budget({1024 * 1024, 64, 65536, 8});
    std::barrier ready(9);
    std::barrier release(9);
    std::atomic_bool bounded = true;
    std::vector<std::jthread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&] {
            std::vector<lanlink::transport::SendBudget::Ticket> pending;
            for (int j = 0; j < 16; ++j) {
                auto ticket = budget.reserve(1024, false);
                if (ticket) pending.push_back(std::move(*ticket));
                const auto usage = budget.usage();
                if (usage.frames > 56 || usage.bytes > 56 * 1024) bounded.store(false);
            }
            ready.arrive_and_wait();
            release.arrive_and_wait();
        });
    }
    ready.arrive_and_wait();
    expect(bounded.load() && budget.usage().frames == 56 && budget.usage().rejected == 72,
           "concurrent senders cannot exceed the shared pending-send bound");
    release.arrive_and_wait();
    workers.clear();
    expect(budget.usage().frames == 0 && budget.usage().bytes == 0,
           "asynchronous completion releases all shared capacity");
}

void test_control_rate() {
    using Limit = lanlink::transport::ControlRateLimit;
    Limit limit(2);
    const auto start = Limit::Clock::time_point{};
    for (int i = 0; i < 8; ++i) expect(limit.allow(start), "control burst allowance");
    expect(!limit.allow(start), "control burst is bounded");
    expect(limit.allow(start + std::chrono::milliseconds(500)), "control allowance replenishes");
    expect(!limit.allow(start), "clock regression does not replenish the allowance");
    expect(limit.allow(start + std::chrono::seconds(1)), "rate resumes after cooldown");
    expect_error([] { Limit invalid(0); }, "zero control request rate");
}

}

int main() {
    test_reconnect_backoff();
    test_send_budget();
    test_concurrent_budget();
    test_control_rate();

    if (failures != 0) {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "all transport tests passed\n";
    return 0;
}
