#include "lanlink/core/component.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    using lanlink::core::Component;

    std::cout << "LanLink " << lanlink::core::component_name(Component::relay)
              << " " << lanlink::core::project_version() << '\n';
    std::cout << "Relay networking is not implemented yet.\n";
    return EXIT_SUCCESS;
}
