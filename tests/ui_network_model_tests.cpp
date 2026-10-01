#include "../apps/ui/network_model.hpp"

#include "lanlink/transport/local_pipe.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

template <typename Predicate>
void wait_until(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("desktop network update timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

}

int main() {
    try {
        namespace protocol = lanlink::protocol;
        protocol::NetworkSummary network;
        network.network_id[0] = std::byte{1};
        network.owner_device_id[0] = std::byte{2};
        network.name = "test-room";
        network.member_count = 1;
        lanlink::transport::LocalPipeServer server([&](
            const protocol::LocalMessage& request) {
            if (request.command == protocol::LocalCommand::list) {
                return protocol::LocalMessage{request.command, true, false,
                    protocol::encode_network_list_result({{network}})};
            }
            if (request.command == protocol::LocalCommand::create) {
                if (protocol::decode_network_create_request(request.payload).name != "new-room") {
                    throw std::runtime_error("wrong network name");
                }
                return protocol::LocalMessage{request.command, true, false,
                    protocol::encode_network_operation_result(
                        {protocol::NetworkOperation::create,
                         protocol::NetworkResultCode::success, network.network_id})};
            }
            throw std::runtime_error("unexpected command");
        });
        lanlink::ui::NetworkModel model;
        wait_until([&] {
            const auto state = model.snapshot();
            return state.service_online && !state.busy &&
                   state.networks.size() == 1 && state.networks[0].name == "test-room";
        });
        model.submit(protocol::LocalCommand::create,
                     protocol::encode_network_create_request({"new-room"}));
        wait_until([&] {
            const auto state = model.snapshot();
            return !state.busy && state.message == "success" &&
                   state.networks.size() == 1;
        });
        server.stop();
        std::cout << "desktop network control validated\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
