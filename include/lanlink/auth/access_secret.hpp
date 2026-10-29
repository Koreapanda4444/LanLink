#pragma once
#include "lanlink/auth/identity.hpp"

namespace lanlink::auth {
[[nodiscard]] DeviceId access_digest(std::string_view secret);
[[nodiscard]] std::string random_access_code();
}
