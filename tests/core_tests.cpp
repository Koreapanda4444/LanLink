#include "lanlink/core/config.hpp"
#include "lanlink/core/logger.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view name) {
    if (!condition) {
        std::cerr << "failed: " << name << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string_view name) {
    try {
        function();
        expect(false, name);
    } catch (const std::exception&) {
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path make_test_directory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("lanlink-core-tests-" + std::to_string(suffix));
    std::filesystem::create_directories(path);
    return path;
}

void test_configuration() {
    const auto config = lanlink::core::parse_config(
        "relay_host=relay.example.com\n"
        "relay_port=8443\n"
        "listen_host=127.0.0.1\n"
        "tls_certificate_file=certs/relay.crt\n"
        "tls_private_key_file=certs/relay.key\n"
        "device_identity_file=data/test.identity\n"
        "auth_token_file=data/test.token\n"
        "quic_handshake_timeout_ms=8000\n"
        "authentication_timeout_ms=7000\n"
        "quic_idle_timeout_ms=90000\n"
        "quic_keep_alive_interval_ms=20000\n"
        "reconnect_initial_delay_ms=500\n"
        "reconnect_max_delay_ms=10000\n"
        "log_level=WARNING\n"
        "log_directory=var/log/lanlink\n"
        "log_max_size_bytes=65536\n"
        "log_max_files=3\n");

    expect(config.relay_host == "relay.example.com", "relay host");
    expect(config.relay_port == 8443, "relay port");
    expect(config.listen_host == "127.0.0.1", "listen host");
    expect(config.tls_certificate_file == std::filesystem::path{"certs/relay.crt"},
           "certificate file");
    expect(config.tls_private_key_file == std::filesystem::path{"certs/relay.key"},
           "private key file");
    expect(config.device_identity_file == std::filesystem::path{"data/test.identity"},
           "device identity file");
    expect(config.auth_token_file == std::filesystem::path{"data/test.token"},
           "auth token file");
    expect(config.quic_handshake_timeout_ms == 8000, "handshake timeout");
    expect(config.authentication_timeout_ms == 7000, "authentication timeout");
    expect(config.quic_idle_timeout_ms == 90000, "idle timeout");
    expect(config.quic_keep_alive_interval_ms == 20000, "keep alive interval");
    expect(config.reconnect_initial_delay_ms == 500, "initial reconnect delay");
    expect(config.reconnect_max_delay_ms == 10000, "maximum reconnect delay");
    expect(config.log_level == lanlink::core::LogLevel::warning, "log level");
    expect(config.log_directory == std::filesystem::path{"var/log/lanlink"}, "log directory");
    expect(config.log_max_size_bytes == 65536, "log size");
    expect(config.log_max_files == 3, "log files");

    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("relay_port=0\n"));
    }, "zero port");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("relay_port=65536\n"));
    }, "large port");
    expect_error([] {
        static_cast<void>(
            lanlink::core::parse_config("relay_host=https://relay.example.com\n"));
    }, "host scheme");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("relay_host=one\nrelay_host=two\n"));
    }, "duplicate key");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("unknown=value\n"));
    }, "unknown key");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("log_max_size_bytes=1024\n"));
    }, "small log size");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("tls_certificate_file=relay.crt\n"));
    }, "incomplete certificate pair");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config(
            "quic_idle_timeout_ms=5000\nquic_keep_alive_interval_ms=5000\n"));
    }, "keep alive shorter than idle timeout");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config("authentication_timeout_ms=999\n"));
    }, "short authentication timeout");
    expect_error([] {
        static_cast<void>(lanlink::core::parse_config(
            "reconnect_initial_delay_ms=1000\nreconnect_max_delay_ms=999\n"));
    }, "reconnect delay order");

    expect_error([] {
        lanlink::core::RuntimeConfig relay_config;
        lanlink::core::validate_relay_config(relay_config);
    }, "relay certificate required");

    try {
        lanlink::core::validate_relay_config(config);
        expect(true, "relay configuration");
    } catch (const std::exception&) {
        expect(false, "relay configuration");
    }

    try {
        lanlink::core::validate_service_config(config);
        expect(true, "service configuration");
    } catch (const std::exception&) {
        expect(false, "service configuration");
    }

    expect_error([] {
        lanlink::core::RuntimeConfig service_config;
        service_config.device_identity_file.clear();
        lanlink::core::validate_service_config(service_config);
    }, "service identity required");
}

void test_configuration_file(const std::filesystem::path& directory) {
    const auto path = directory / "lanlink.conf";

    {
        std::ofstream output(path, std::ios::binary);
        output << "relay_host=10.0.0.1\nrelay_port=4433\n";
    }

    const auto config = lanlink::core::load_config(path);
    expect(config.relay_host == "10.0.0.1", "load config host");
    expect(config.relay_port == 4433, "load config port");

    expect_error([&directory] {
        static_cast<void>(lanlink::core::load_config(directory / "missing.conf"));
    }, "missing config");
}

void test_log_rotation(const std::filesystem::path& directory) {
    const auto path = directory / "rotation.log";

    {
        lanlink::core::Logger logger(path, "test", lanlink::core::LogLevel::trace, 220, 3);

        for (int index = 0; index < 24; ++index) {
            logger.info("rotation-entry-" + std::to_string(index) + "-abcdefghijklmnopqrstuvwxyz");
        }

        logger.warning("quoted \"value\"\nnext");
        logger.flush();
    }

    expect(std::filesystem::exists(path), "active log");
    expect(std::filesystem::exists(path.string() + ".1"), "first rotated log");
    expect(std::filesystem::exists(path.string() + ".2"), "second rotated log");
    expect(!std::filesystem::exists(path.string() + ".3"), "rotation file limit");

    const auto active = read_file(path);
    expect(active.find("\"level\":\"warn\"") != std::string::npos, "structured level");
    expect(active.find("\"component\":\"test\"") != std::string::npos,
           "structured component");
    expect(active.find("quoted \\\"value\\\"\\nnext") != std::string::npos, "json escaping");
}

void test_log_filtering(const std::filesystem::path& directory) {
    const auto path = directory / "filter.log";

    {
        lanlink::core::Logger logger(path, "filter", lanlink::core::LogLevel::warning, 4096, 2);
        logger.debug("hidden-message");
        logger.error("visible-message");
        logger.flush();
    }

    const auto content = read_file(path);
    expect(content.find("hidden-message") == std::string::npos, "filtered message");
    expect(content.find("visible-message") != std::string::npos, "visible message");
}

void test_concurrent_logging(const std::filesystem::path& directory) {
    const auto path = directory / "concurrent.log";

    {
        lanlink::core::Logger logger(path, "concurrent", lanlink::core::LogLevel::info, 1024 * 1024, 2);
        std::vector<std::thread> threads;

        for (int thread_index = 0; thread_index < 4; ++thread_index) {
            threads.emplace_back([&logger, thread_index] {
                for (int entry = 0; entry < 50; ++entry) {
                    logger.info("thread-" + std::to_string(thread_index) + "-entry-" +
                                std::to_string(entry));
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        logger.flush();
    }

    const auto content = read_file(path);
    expect(std::count(content.begin(), content.end(), '\n') == 200, "concurrent records");
}

}

int main() {
    const auto directory = make_test_directory();

    try {
        test_configuration();
        test_configuration_file(directory);
        test_log_rotation(directory);
        test_log_filtering(directory);
        test_concurrent_logging(directory);
    } catch (const std::exception& error) {
        std::cerr << "unexpected error: " << error.what() << '\n';
        ++failures;
    }

    std::error_code error;
    std::filesystem::remove_all(directory, error);

    if (error) {
        std::cerr << "cleanup failed: " << error.message() << '\n';
        ++failures;
    }

    if (failures != 0) {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
