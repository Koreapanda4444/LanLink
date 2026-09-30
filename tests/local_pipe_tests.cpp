#include "lanlink/transport/local_pipe.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        using namespace lanlink;
        transport::LocalPipeServer server([](const protocol::LocalMessage& request) {
            if (request.command != protocol::LocalCommand::status) {
                throw std::invalid_argument("unsupported command");
            }
            return protocol::LocalMessage{request.command, true, false,
                {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                 std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}};
        });
        const auto response = transport::local_pipe_request(
            {protocol::LocalCommand::status, false, false, {}});
        if (response.payload.size() != 8 || response.payload[0] != std::byte{1}) {
            throw std::runtime_error("local pipe response mismatch");
        }
        bool rejected = false;
        try {
            static_cast<void>(transport::local_pipe_request(
                {protocol::LocalCommand::list, false, false, {}}));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!rejected) throw std::runtime_error("local pipe error was not returned");
        server.stop();
        std::cout << "local pipe requests validated\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
