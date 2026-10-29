#pragma once
#include "lanlink/auth/identity.hpp"
#include "lanlink/core/config.hpp"
#include <functional>

namespace lanlink::auth {
struct RelayEndpoint {
    std::string id;
    std::string host;
    std::uint16_t port = 4433;
    bool operator==(const RelayEndpoint&) const = default;
};
struct DirectoryCatalog {
    std::uint64_t issued_ms = 0;
    std::uint64_t expires_ms = 0;
    std::vector<RelayEndpoint> relays;
};
[[nodiscard]] std::vector<std::byte> sign_directory(const DirectoryCatalog& catalog, const DeviceIdentity& identity);
[[nodiscard]] DirectoryCatalog verify_directory(std::span<const std::byte> response, const PublicKey& trusted_key, std::uint64_t now_ms);
[[nodiscard]] PublicKey parse_directory_key(std::string_view hex);
using DirectoryFetcher = std::function<std::vector<std::byte>(const std::string&)>;
struct BootstrapResult {
    RelayEndpoint relay;
    bool from_directory = false;
    std::string error;
};
[[nodiscard]] BootstrapResult resolve_bootstrap(const core::RuntimeConfig& config, const DirectoryFetcher& fetch, std::uint64_t now_ms);
}
