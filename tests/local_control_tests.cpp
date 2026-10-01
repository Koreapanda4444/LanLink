#include "lanlink/protocol/local_control.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(const bool condition) {
    if (!condition) throw std::runtime_error("local control test failed");
}

template <typename Action>
void rejects(Action action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid local control frame accepted");
}

}

int main() {
    try {
        using namespace lanlink::protocol;
        const LocalMessage request{LocalCommand::create, false, false,
                                   {std::byte{0}, std::byte{1}, std::byte{255}}};
        auto frame = encode_local_message(request);
        require(decode_local_message(frame) == request);
        require(decode_local_message(encode_local_message(
            {LocalCommand::status, true, false, {}})) ==
            (LocalMessage{LocalCommand::status, true, false, {}}));
        rejects([&] { static_cast<void>(decode_local_message(
            std::vector<std::byte>(frame.begin(), frame.end() - 1))); });
        frame[0] = std::byte{0};
        rejects([&] { static_cast<void>(decode_local_message(frame)); });
        frame = encode_local_message(request);
        frame[4] = std::byte{2};
        rejects([&] { static_cast<void>(decode_local_message(frame)); });
        frame = encode_local_message(request);
        frame[6] = std::byte{2};
        rejects([&] { static_cast<void>(decode_local_message(frame)); });
        frame = encode_local_message(request);
        frame[11] = std::byte{255};
        rejects([&] { static_cast<void>(decode_local_message(frame)); });
        rejects([&] { static_cast<void>(encode_local_message(
            {LocalCommand::status, false, false,
             std::vector<std::byte>(max_local_control_payload + 1)})); });
        LocalDiagnostics diagnostics;
        diagnostics.connected = true;
        diagnostics.authenticated = true;
        diagnostics.active_networks = 2;
        diagnostics.rtt_us = 1350;
        diagnostics.sent_bytes = 123456;
        diagnostics.received_bytes = 654321;
        diagnostics.reconnects = 4;
        diagnostics.last_error = "relay timeout";
        auto metrics = encode_local_diagnostics(diagnostics);
        require(decode_local_diagnostics(metrics) == diagnostics);
        metrics[3] = std::byte{1};
        rejects([&] { static_cast<void>(decode_local_diagnostics(metrics)); });
        metrics = encode_local_diagnostics(diagnostics);
        metrics.pop_back();
        rejects([&] { static_cast<void>(decode_local_diagnostics(metrics)); });
        std::cout << "local control frames validated\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
