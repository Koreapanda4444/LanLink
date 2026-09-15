#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/quic.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

}

int main(const int argc, char* argv[]) {
    using lanlink::core::Component;

    try {
        if (argc > 2) {
            throw std::invalid_argument("usage: lanlink-relay [config-path]");
        }

        auto config = argc == 2
                          ? lanlink::core::load_config(std::filesystem::path{argv[1]})
                          : lanlink::core::RuntimeConfig{};
        lanlink::core::validate_relay_config(config);

        lanlink::core::Logger logger(config.log_directory / "relay.log",
                                     "relay",
                                     config.log_level,
                                     config.log_max_size_bytes,
                                     config.log_max_files);
        logger.info("starting");
        std::cout << "LanLink " << lanlink::core::component_name(Component::relay)
                  << " " << lanlink::core::project_version() << '\n';

        lanlink::transport::QuicServerOptions options;
        options.listen_host = config.listen_host;
        options.port = config.relay_port;
        options.certificate_file = config.tls_certificate_file;
        options.private_key_file = config.tls_private_key_file;
        options.auth_token_file = config.auth_token_file;
        options.handshake_timeout = std::chrono::milliseconds{config.quic_handshake_timeout_ms};
        options.idle_timeout = std::chrono::milliseconds{config.quic_idle_timeout_ms};
        options.keep_alive_interval_ms = config.quic_keep_alive_interval_ms;

        lanlink::transport::QuicRelayServer server(std::move(options), logger);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        server.start();

        while (stop_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }

        server.stop();
        logger.info("stopped");
        logger.flush();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-relay: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
