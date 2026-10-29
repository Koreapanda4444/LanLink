#include "lanlink/network/multicast.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"

#include <algorithm>

namespace lanlink::network {
namespace {
std::uint32_t integer(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < size; ++index)
        value = (value << 8U) | std::to_integer<unsigned char>(bytes[offset + index]);
    return value;
}
bool checksum(std::span<const std::byte> bytes) {
    std::uint32_t sum = 0;
    for (std::size_t index = 0; index < bytes.size(); index += 2) {
        sum += std::to_integer<unsigned char>(bytes[index]) << 8U;
        if (index + 1 < bytes.size()) sum += std::to_integer<unsigned char>(bytes[index + 1]);
    }
    while (sum >> 16U) sum = (sum & 0xffffU) + (sum >> 16U);
    return sum == 0xffffU;
}
}

MulticastMembership::Report MulticastMembership::observe(
    std::span<const std::byte> ipv4, const Clock::time_point now) {
    if (ipv4.size() < 20 || ipv4[9] != std::byte{2}) return Report::not_igmp;
    const auto header = (std::to_integer<unsigned char>(ipv4[0]) & 15U) * 4U;
    if ((std::to_integer<unsigned char>(ipv4[0]) >> 4U) != 4 || header < 20 ||
        header > ipv4.size() || integer(ipv4, 2, 2) != ipv4.size() ||
        (integer(ipv4, 6, 2) & 0x3fffU) != 0 || ipv4[8] != std::byte{1}) return Report::rejected;
    const auto payload = ipv4.subspan(header);
    if (payload.size() < 8 || !checksum(payload)) return Report::rejected;
    auto updated = groups_;
    std::erase_if(updated, [now](const auto& entry) { return now - entry.second.updated > std::chrono::seconds{260}; });
    const auto type = std::to_integer<unsigned char>(payload[0]);
    const auto destination = integer(ipv4, 16, 4);
    if (type == 0x12 || type == 0x16 || type == 0x17) {
        const auto group = integer(payload, 4, 4);
        if (payload.size() != 8 || !protocol::is_ipv4_scoped_multicast(group) ||
            destination != (type == 0x17 ? 0xe0000002U : group)) return Report::rejected;
        updated[group] = Group{type != 0x17, {}, now};
    } else if (type == 0x22) {
        const auto count = integer(payload, 6, 2);
        if (destination != 0xe0000016U || count > 64) return Report::rejected;
        std::size_t offset = 8;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (payload.size() - offset < 8) return Report::rejected;
            const auto record = integer(payload, offset, 1);
            const auto auxiliary = integer(payload, offset + 1, 1) * 4U;
            const auto sources = integer(payload, offset + 2, 2);
            const auto group = integer(payload, offset + 4, 4);
            if (record < 1 || record > 6 || sources > 64 ||
                !protocol::is_ipv4_scoped_multicast(group) ||
                8U + sources * 4U + auxiliary > payload.size() - offset) return Report::rejected;
            std::set<std::uint32_t> addresses;
            for (std::uint32_t source = 0; source < sources; ++source) {
                const auto address = integer(payload, offset + 8 + source * 4U, 4);
                if (address == 0 || address >= 0xe0000000U) return Report::rejected;
                addresses.insert(address);
            }
            auto& entry = updated[group];
            if (record <= 4) { entry.exclude = record == 2 || record == 4; entry.sources = std::move(addresses); }
            else for (const auto address : addresses) {
                if ((record == 5) != entry.exclude) entry.sources.insert(address);
                else entry.sources.erase(address);
            }
            if (entry.sources.size() > 64) return Report::rejected;
            entry.updated = now;
            offset += 8U + sources * 4U + auxiliary;
        }
        if (offset != payload.size()) return Report::rejected;
    } else return Report::rejected;
    if (updated.size() > 64) return Report::rejected;
    groups_.swap(updated);
    return Report::accepted;
}

bool MulticastMembership::receives(const std::uint32_t group, const std::uint32_t source,
                                    const Clock::time_point now) const {
    if (!protocol::is_ipv4_scoped_multicast(group)) return false;
    if (host_groups_) return host_groups_->contains(group);
    const auto found = groups_.find(group);
    if (found != groups_.end() && now - found->second.updated <= std::chrono::seconds{260})
        return found->second.exclude != found->second.sources.contains(source);
    return group == 0xe00000fbU || group == 0xeffffffaU;
}

void MulticastMembership::synchronize(const std::optional<std::vector<std::uint32_t>>& host_groups) {
    host_groups_.reset();
    if (!host_groups) return;
    host_groups_.emplace();
    for (const auto group : *host_groups)
        if (protocol::is_ipv4_scoped_multicast(group) && host_groups_->size() < 256) host_groups_->insert(group);
}
}
