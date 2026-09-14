#include "lanlink/core/component.hpp"

#ifndef LANLINK_VERSION
#define LANLINK_VERSION "0.1.0-dev"
#endif

namespace lanlink::core {

std::string_view component_name(const Component component) noexcept {
    switch (component) {
        case Component::relay:
            return "relay";
        case Component::service:
            return "service";
        case Component::ui:
            return "ui";
    }

    return "unknown";
}

std::string_view project_version() noexcept {
    return LANLINK_VERSION;
}

}
