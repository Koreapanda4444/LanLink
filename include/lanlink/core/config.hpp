#pragma once

#include "lanlink/core/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lanlink::core {

struct RuntimeConfig {
    std::string relay_host = "127.0.0.1";
    std::uint16_t relay_port = 4433;
    std::string listen_host = "0.0.0.0";
    LogLevel log_level = LogLevel::info;
    std::filesystem::path log_directory = "logs";
    std::uintmax_t log_max_size_bytes = 5U * 1024U * 1024U;
    std::size_t log_max_files = 5;
};

[[nodiscard]] RuntimeConfig parse_config(std::string_view content);
[[nodiscard]] RuntimeConfig load_config(const std::filesystem::path& path);
void validate_config(const RuntimeConfig& config);

}
