#include "lanlink/core/config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace lanlink::core {
namespace {

constexpr std::uintmax_t min_log_size = 64U * 1024U;
constexpr std::uintmax_t max_log_size = 1024U * 1024U * 1024U;
constexpr std::size_t max_config_size = 1024U * 1024U;

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }

    return value;
}

[[noreturn]] void fail(const std::size_t line, const std::string& message) {
    throw std::runtime_error("configuration line " + std::to_string(line) + ": " + message);
}

std::uint64_t parse_unsigned(std::string_view value,
                             std::string_view key,
                             const std::size_t line) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);

    if (error != std::errc{} || end != value.data() + value.size()) {
        fail(line, std::string(key) + " must be an unsigned integer");
    }

    return result;
}

bool invalid_host(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.find("://") != std::string_view::npos ||
        host.find('/') != std::string_view::npos || host.find('\\') != std::string_view::npos) {
        return true;
    }

    return std::any_of(host.begin(), host.end(), [](const char value) {
        return std::isspace(static_cast<unsigned char>(value)) != 0;
    });
}

}

RuntimeConfig parse_config(const std::string_view content) {
    if (content.size() > max_config_size) {
        throw std::runtime_error("configuration exceeds 1 MiB");
    }

    RuntimeConfig config;
    std::unordered_set<std::string> keys;
    std::size_t offset = 0;
    std::size_t line_number = 1;

    while (offset < content.size()) {
        const auto end = content.find('\n', offset);
        auto line = content.substr(offset, end == std::string_view::npos ? content.size() - offset
                                                                        : end - offset);

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        line = trim(line);

        if (!line.empty() && line.front() != '#') {
            const auto separator = line.find('=');

            if (separator == std::string_view::npos) {
                fail(line_number, "expected key=value");
            }

            const auto key_view = trim(line.substr(0, separator));
            const auto value = trim(line.substr(separator + 1));

            if (key_view.empty()) {
                fail(line_number, "key is empty");
            }

            if (value.empty()) {
                fail(line_number, std::string(key_view) + " is empty");
            }

            const std::string key(key_view);

            if (!keys.insert(key).second) {
                fail(line_number, "duplicate key " + key);
            }

            if (key == "relay_host") {
                config.relay_host = value;
            } else if (key == "relay_port") {
                const auto parsed = parse_unsigned(value, key_view, line_number);

                if (parsed > std::numeric_limits<std::uint16_t>::max()) {
                    fail(line_number, "relay_port is out of range");
                }

                config.relay_port = static_cast<std::uint16_t>(parsed);
            } else if (key == "listen_host") {
                config.listen_host = value;
            } else if (key == "log_level") {
                try {
                    config.log_level = parse_log_level(value);
                } catch (const std::invalid_argument&) {
                    fail(line_number, "invalid log_level");
                }
            } else if (key == "log_directory") {
                config.log_directory = std::string(value);
            } else if (key == "log_max_size_bytes") {
                config.log_max_size_bytes = parse_unsigned(value, key_view, line_number);
            } else if (key == "log_max_files") {
                const auto parsed = parse_unsigned(value, key_view, line_number);

                if (parsed > std::numeric_limits<std::size_t>::max()) {
                    fail(line_number, "log_max_files is out of range");
                }

                config.log_max_files = static_cast<std::size_t>(parsed);
            } else {
                fail(line_number, "unknown key " + key);
            }
        }

        if (end == std::string_view::npos) {
            break;
        }

        offset = end + 1;
        ++line_number;
    }

    validate_config(config);
    return config;
}

RuntimeConfig load_config(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);

    if (!input) {
        throw std::runtime_error("cannot open configuration: " + path.string());
    }

    const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

    if (!input.eof() && input.fail()) {
        throw std::runtime_error("cannot read configuration: " + path.string());
    }

    return parse_config(content);
}

void validate_config(const RuntimeConfig& config) {
    if (invalid_host(config.relay_host)) {
        throw std::runtime_error("relay_host is invalid");
    }

    if (config.relay_port == 0) {
        throw std::runtime_error("relay_port must be between 1 and 65535");
    }

    if (invalid_host(config.listen_host)) {
        throw std::runtime_error("listen_host is invalid");
    }

    if (config.log_directory.empty()) {
        throw std::runtime_error("log_directory is empty");
    }

    if (config.log_max_size_bytes < min_log_size || config.log_max_size_bytes > max_log_size) {
        throw std::runtime_error("log_max_size_bytes must be between 65536 and 1073741824");
    }

    if (config.log_max_files == 0 || config.log_max_files > 32) {
        throw std::runtime_error("log_max_files must be between 1 and 32");
    }
}

}
