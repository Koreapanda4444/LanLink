#include "lanlink/protocol/game_diagnostics.hpp"
#include "lanlink/protocol/local_control.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Operation> void rejects(Operation operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid game diagnostics accepted");
}
}
int main() {
    try {
        namespace p = lanlink::protocol;
        p::GameDiagnostics value;
        value.networks.push_back({{std::byte{1}}, 0x0a400001U, 0x0a400000U, 24, 2, 4, 6, 1, 2, 3, 5});
        value.rejected_packets = 9; value.adapter_failed = true;
        const auto bytes = p::encode_game_diagnostics(value);
        require(p::decode_game_diagnostics(bytes) == value, "game diagnostic round trip changed telemetry");
        for (std::size_t size = 0; size < bytes.size(); ++size)
            rejects([&] { static_cast<void>(p::decode_game_diagnostics(std::span(bytes).first(size))); });
        auto trailing = bytes; trailing.push_back(std::byte{0});
        rejects([&] { static_cast<void>(p::decode_game_diagnostics(trailing)); });
        auto invalid = value; invalid.mtu = 0;
        rejects([&] { static_cast<void>(p::encode_game_diagnostics(invalid)); });
        invalid = value; invalid.networks.push_back(invalid.networks.front());
        rejects([&] { static_cast<void>(p::encode_game_diagnostics(invalid)); });
        invalid = value; invalid.networks[0].prefix_length = 0;
        rejects([&] { static_cast<void>(p::encode_game_diagnostics(invalid)); });
        require(p::decode_local_message(p::encode_local_message({p::LocalCommand::game_diagnostics,
            true, false, bytes})).payload == bytes, "game diagnostics local command rejected");
        std::cout << "Game telemetry bounds and local protocol validated\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
