#include "lanlink/protocol/capabilities.hpp"
#include "lanlink/protocol/codec.hpp"
#include "lanlink/relay/network_control_channel.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace lanlink;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejected(F action) {
    bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, "malformed capability offer was accepted");
}
void scenario(const std::filesystem::path& directory) {
    protocol::CapabilityOffer offer;
    offer.maximum_version = 7;
    offer.features = ~std::uint64_t{0};
    const auto encoded = protocol::encode_capability_offer(offer);
    require(protocol::decode_capability_offer(encoded) == offer, "capability offer did not round trip");
    const auto selected = protocol::negotiate_capabilities({}, offer);
    require(!selected.legacy && selected.wire_version == 5 &&
            selected.features == protocol::supported_capabilities, "unsupported features were negotiated");
    require(protocol::decode_capability_selection(protocol::encode_capability_selection(selected)) == selected,
            "capability selection did not round trip");
    require(protocol::negotiate_capabilities({}, {6, 9, ~std::uint64_t{0}}).legacy,
            "incompatible versions did not fall back safely");
    require(protocol::negotiate_capabilities({5, 5, 1}, {5, 5, 8}).features == 0,
            "feature mismatch enabled a capability");
    for (std::size_t size = 0; size < encoded.size(); ++size)
        rejected([&] { static_cast<void>(protocol::decode_capability_offer(std::span(encoded).first(size))); });
    rejected([&] { static_cast<void>(protocol::encode_capability_offer({7, 5, 1})); });
    auto wrong_schema = encoded; wrong_schema[4] = std::byte{2};
    rejected([&] { static_cast<void>(protocol::decode_capability_offer(wrong_schema)); });
    rejected([&] { static_cast<void>(protocol::decode_network_list_request(encoded)); });
    const protocol::Frame probe{protocol::MessageType::network_list_request,
                                protocol::capability_request_id, encoded};
    require(protocol::decode_frame(protocol::encode_frame(probe)).frame == probe,
            "capability discovery changed the legacy frame envelope");
    storage::RelayStore store(directory / "relay.db");
    relay::NetworkService service(store);
    relay::NetworkControlChannel channel(service);
    auth::DeviceId actor{}; actor[0] = std::byte{1};
    require(channel.handle_authenticated(actor, probe).response.type == protocol::MessageType::error,
            "unregistered actor negotiated capabilities");
    channel.note_authenticated(actor);
    const auto reply = channel.handle_authenticated(actor, probe).response;
    require(reply.request_id == protocol::capability_request_id &&
            protocol::decode_capability_selection(reply.payload) == selected &&
            channel.capabilities(actor) == selected, "authenticated negotiation was not session-bound");
    channel.note_authenticated(actor);
    require(channel.capabilities(actor).legacy, "reconnection retained previous capabilities");
    const protocol::Frame old_request{protocol::MessageType::network_list_request, 42,
                                     protocol::encode_network_list_request()};
    require(channel.handle_authenticated(actor, old_request).response.type == protocol::MessageType::network_list_result &&
            channel.capabilities(actor).legacy, "legacy list request required capability negotiation");
}
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-capabilities-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory); scenario(directory); std::filesystem::remove_all(directory);
        std::cout << "capability negotiation tests passed\n"; return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; std::error_code ignored; std::filesystem::remove_all(directory, ignored); return 1;
    }
}
