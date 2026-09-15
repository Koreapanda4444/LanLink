#pragma once

#include "lanlink/auth/identity.hpp"

#include <filesystem>

namespace lanlink::auth::detail {

[[nodiscard]] bool load_device_secret(const std::filesystem::path& path,
                                      SecretKey& secret);
void store_device_secret(const std::filesystem::path& path,
                         const SecretKey& secret);

}
