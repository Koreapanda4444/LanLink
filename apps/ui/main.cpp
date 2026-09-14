#include "lanlink/core/component.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    using lanlink::core::Component;

    std::cout << "LanLink " << lanlink::core::component_name(Component::ui)
              << " " << lanlink::core::project_version() << '\n';
    std::cout << "Dear ImGui integration is not implemented yet.\n";
    return EXIT_SUCCESS;
}
