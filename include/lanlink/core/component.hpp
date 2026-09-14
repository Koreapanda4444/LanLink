#pragma once

#include <string_view>

namespace lanlink::core {

enum class Component {
    relay,
    service,
    ui,
};

[[nodiscard]] std::string_view component_name(Component component) noexcept;
[[nodiscard]] std::string_view project_version() noexcept;

}
