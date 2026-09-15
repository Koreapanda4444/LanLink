#include "lanlink/transport/reconnect.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <string_view>

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

}

int main() {
    test_reconnect_backoff();

    if (failures != 0) {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "all transport tests passed\n";
    return 0;
}
