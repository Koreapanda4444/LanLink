#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(const int argc, char* argv[]) {
    using lanlink::core::Component;

    try {
        if (argc > 2) {
            throw std::invalid_argument("usage: lanlink-ui [config-path]");
        }

        auto config = argc == 2
                          ? lanlink::core::load_config(std::filesystem::path{argv[1]})
                          : lanlink::core::RuntimeConfig{};
        lanlink::core::validate_config(config);

        lanlink::core::Logger logger(config.log_directory / "ui.log",
                                     "ui",
                                     config.log_level,
                                     config.log_max_size_bytes,
                                     config.log_max_files);
        logger.info("starting");
        std::cout << "LanLink " << lanlink::core::component_name(Component::ui)
                  << " " << lanlink::core::project_version() << '\n';
        std::cout << "Dear ImGui integration is not implemented yet.\n";
        logger.info("stopped");
        logger.flush();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-ui: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
