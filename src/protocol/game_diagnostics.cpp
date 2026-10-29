#include "lanlink/protocol/game_diagnostics.hpp"
#include "lanlink/protocol/binary.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"
#include <set>

namespace lanlink::protocol {
namespace {
void validate(const GameDiagnostics& value) {
    if (value.mtu < 576 || value.mtu > 9000 || value.networks.size() > max_game_diagnostic_networks)
        throw std::invalid_argument("invalid game diagnostic limits");
    std::set<NetworkId> ids;
    for (const auto& item : value.networks) {
        if (!is_ipv4_host(item.address, item.subnet, item.prefix_length) ||
            !ids.insert(item.network_id).second || item.network_id == NetworkId{})
            throw std::invalid_argument("invalid game diagnostic network");
    }
}
}
std::vector<std::byte> encode_game_diagnostics(const GameDiagnostics& value) {
    validate(value);
    BinaryWriter writer;
    writer.put(std::uint32_t{0x4c4c4744}); writer.put(std::uint8_t{1});
    writer.put(static_cast<std::uint8_t>((value.adapter_failed ? 1 : 0) | (value.truncated ? 2 : 0)));
    writer.put(value.mtu); writer.put(value.rejected_packets);
    writer.put(static_cast<std::uint16_t>(value.networks.size()));
    for (const auto& item : value.networks) {
        writer.append(item.network_id); writer.put(item.address); writer.put(item.subnet);
        writer.put(item.prefix_length); writer.put(item.online_peers);
        writer.put(item.discovery_sent); writer.put(item.discovery_received);
        writer.put(item.rate_limited); writer.put(item.duplicates);
        writer.put(item.invalid); writer.put(item.reflections);
    }
    return std::move(writer).finish();
}
GameDiagnostics decode_game_diagnostics(std::span<const std::byte> bytes) {
    BinaryReader reader(bytes);
    if (reader.get<std::uint32_t>() != 0x4c4c4744 || reader.get<std::uint8_t>() != 1)
        throw std::invalid_argument("invalid game diagnostic header");
    const auto flags = reader.get<std::uint8_t>();
    if (flags > 3) throw std::invalid_argument("invalid game diagnostic flags");
    GameDiagnostics value; value.adapter_failed = (flags & 1) != 0; value.truncated = (flags & 2) != 0;
    value.mtu = reader.get<std::uint16_t>(); value.rejected_packets = reader.get<std::uint64_t>();
    const auto count = reader.get<std::uint16_t>();
    if (count > max_game_diagnostic_networks) throw std::invalid_argument("too many game diagnostic networks");
    for (std::size_t i = 0; i < count; ++i) {
        GameNetworkStatus item;
        item.network_id = reader.array<network_id_size>();
        item.address = reader.get<std::uint32_t>(); item.subnet = reader.get<std::uint32_t>();
        item.prefix_length = reader.get<std::uint8_t>(); item.online_peers = reader.get<std::uint32_t>();
        item.discovery_sent = reader.get<std::uint64_t>(); item.discovery_received = reader.get<std::uint64_t>();
        item.rate_limited = reader.get<std::uint64_t>(); item.duplicates = reader.get<std::uint64_t>();
        item.invalid = reader.get<std::uint64_t>(); item.reflections = reader.get<std::uint64_t>();
        value.networks.push_back(item);
    }
    reader.finish(); validate(value); return value;
}
}
