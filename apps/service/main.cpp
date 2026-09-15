#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/transport/quic.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
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
            throw std::invalid_argument("usage: lanlink-service [config-path]");
        }

        auto config = argc == 2
                          ? lanlink::core::load_config(std::filesystem::path{argv[1]})
                          : lanlink::core::RuntimeConfig{};
        lanlink::core::validate_config(config);

        lanlink::core::Logger logger(config.log_directory / "service.log",
                                     "service",
                                     config.log_level,
                                     config.log_max_size_bytes,
                                     config.log_max_files);
        logger.info("starting");
        std::cout << "LanLink " << lanlink::core::component_name(Component::service)
                  << " " << lanlink::core::project_version() << '\n';

        lanlink::transport::QuicClientOptions options;
        options.relay_host = config.relay_host;
        options.port = config.relay_port;
        options.handshake_timeout = std::chrono::milliseconds{config.quic_handshake_timeout_ms};
        options.idle_timeout = std::chrono::milliseconds{config.quic_idle_timeout_ms};
        options.keep_alive_interval_ms = config.quic_keep_alive_interval_ms;
        options.reconnect_initial_delay =
            std::chrono::milliseconds{config.reconnect_initial_delay_ms};
        options.reconnect_maximum_delay =
            std::chrono::milliseconds{config.reconnect_max_delay_ms};

        lanlink::transport::QuicRelayClient client(std::move(options), logger);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        std::atomic_bool worker_finished = false;
        std::exception_ptr worker_error;
        std::jthread worker([&](const std::stop_token token) {
            try {
                client.run(token);
            } catch (...) {
                worker_error = std::current_exception();
            }

            worker_finished.store(true);
        });

        while (stop_requested == 0 && !worker_finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }

        worker.request_stop();
        client.stop();
        worker.join();

        if (worker_error) {
            std::rethrow_exception(worker_error);
        }
        logger.info("stopped");
        logger.flush();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-service: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
