#pragma once
#include "lanlink/auth/identity.hpp"
#include "lanlink/core/config.hpp"
#include "lanlink/protocol/message.hpp"
#include <functional>

namespace lanlink::auth {
enum class DirectoryScope : std::uint8_t { legacy, official, self_hosted };
struct RelayEndpoint {
    std::string id;
    std::string host;
    std::uint16_t port = 4433;
    std::string region = {};
    std::uint32_t capacity = 0;
    std::uint16_t protocol_version = protocol::protocol_version;
    std::string authority = {};
    PublicKey health_key{};
    bool operator==(const RelayEndpoint&) const = default;
};
struct DirectoryCatalog {
    std::uint64_t issued_ms = 0;
    std::uint64_t expires_ms = 0;
    std::vector<RelayEndpoint> relays;
    DirectoryScope scope = DirectoryScope::legacy;
    std::uint64_t revision = 0;
};
[[nodiscard]] std::vector<std::byte> sign_directory(const DirectoryCatalog& catalog, const DeviceIdentity& identity);
[[nodiscard]] DirectoryCatalog verify_directory(std::span<const std::byte> response, const PublicKey& trusted_key, std::uint64_t now_ms);
[[nodiscard]] PublicKey parse_directory_key(std::string_view hex);
using DirectoryFetcher = std::function<std::vector<std::byte>(const std::string&)>;
struct BootstrapResult {
    RelayEndpoint relay;
    bool from_directory = false;
    std::string error;
    std::vector<RelayEndpoint> pool = {};
    std::uint64_t catalog_expires_ms = 0;
    std::uint64_t catalog_revision = 0;
};
[[nodiscard]] BootstrapResult resolve_bootstrap(const core::RuntimeConfig& config, const DirectoryFetcher& fetch, std::uint64_t now_ms);
}
