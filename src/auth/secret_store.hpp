#pragma once

#include "lanlink/auth/identity.hpp"

#include <filesystem>

namespace lanlink::auth::detail {
std::vector<std::byte> read_private_blob(const std::filesystem::path& path);
void write_private_blob(const std::filesystem::path& path, std::span<const std::byte> bytes);

[[nodiscard]] bool load_device_secret(const std::filesystem::path& path,
                                      SecretKey& secret);
void store_device_secret(const std::filesystem::path& path,
                         const SecretKey& secret);

}
