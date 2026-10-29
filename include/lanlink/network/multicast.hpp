#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace lanlink::network {

class MulticastMembership {
public:
    using Clock = std::chrono::steady_clock;
    enum class Report { not_igmp, accepted, rejected };
    [[nodiscard]] Report observe(std::span<const std::byte> ipv4, Clock::time_point now);
    [[nodiscard]] bool receives(std::uint32_t group, std::uint32_t source, Clock::time_point now) const;
    void synchronize(const std::optional<std::vector<std::uint32_t>>& host_groups);

private:
    struct Group {
        bool exclude = false;
        std::set<std::uint32_t> sources;
        Clock::time_point updated{};
    };
    std::map<std::uint32_t, Group> groups_;
    std::optional<std::set<std::uint32_t>> host_groups_;
};

}
