#pragma once
#include "lanlink/auth/identity.hpp"

namespace lanlink::auth {
struct PasswordVerifier {
    Nonce salt{};
    DeviceId digest{};
    bool require_approval = false;
};
[[nodiscard]] PasswordVerifier protect_password(std::string_view password, bool require_approval);
[[nodiscard]] bool password_matches(std::string_view password, const PasswordVerifier& verifier);
[[nodiscard]] DeviceId access_digest(std::string_view secret);
[[nodiscard]] std::string random_access_code();
}
