#pragma once

#include <filesystem>
#include <functional>
#include <optional>

namespace lanlink::service {

using Runtime = std::function<void(const std::optional<std::filesystem::path>&,
                                   const std::function<void()>&)>;

void install(std::optional<std::filesystem::path> config_path);
void uninstall();
void start();
void stop();
void dispatch(std::optional<std::filesystem::path> config_path,
              const std::filesystem::path& working_directory,
              Runtime runtime);
[[nodiscard]] bool stop_requested() noexcept;

}
