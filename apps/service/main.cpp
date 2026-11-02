#include "windows_service.hpp"
#include "lanlink/platform/service_runtime.hpp"
#include "lanlink/core/config.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
volatile std::sig_atomic_t console_stop_requested = 0;
void handle_signal(int) { console_stop_requested = 1; }
void run_runtime(const std::optional<std::filesystem::path>& path, const std::function<void()>& ready) {
    lanlink::service::run_client(path, {ready,
        [] { return console_stop_requested != 0 || lanlink::service::stop_requested(); },
        [] { lanlink::service::request_stop(); }});
}

}

int wmain(const int argc, wchar_t* argv[]) {
    try {
        const auto action = argc > 1 ? std::wstring_view{argv[1]} : std::wstring_view{};
        if (action == L"--install" && argc <= 3) {
            const std::optional<std::filesystem::path> path = argc == 3
                ? std::optional<std::filesystem::path>{argv[2]} : std::nullopt;
            if (path) {
                lanlink::core::validate_service_config(lanlink::core::load_config(*path));
            }
            lanlink::service::install(path);
            std::cout << "LanLink service installed for automatic startup\n";
        } else if (action == L"--uninstall" && argc == 2) {
            lanlink::service::uninstall();
            std::cout << "LanLink service uninstalled\n";
        } else if (action == L"--start" && argc == 2) {
            lanlink::service::start();
            std::cout << "LanLink service running\n";
        } else if (action == L"--stop" && argc == 2) {
            lanlink::service::stop();
            std::cout << "LanLink service stopped\n";
        } else if (action == L"--create-shortcut" && argc == 3) {
            lanlink::service::create_shortcut(argv[2]);
            std::cout << "LanLink desktop shortcut created\n";
        } else if (action == L"--service" && argc == 4) {
            const std::optional<std::filesystem::path> path =
                std::wstring_view{argv[2]} == L"-"
                    ? std::nullopt
                    : std::optional<std::filesystem::path>{argv[2]};
            lanlink::service::dispatch(path, std::filesystem::path{argv[3]},
                [](const auto& config, const auto& ready) {
                    run_runtime(config, ready);
                });
        } else if ((action == L"--console" && argc <= 3) ||
                   (action.empty() && argc == 1) ||
                   (argc == 2 && !action.starts_with(L"--"))) {
            const std::optional<std::filesystem::path> path =
                argc == 1 || (action == L"--console" && argc == 2)
                    ? std::nullopt
                    : std::optional<std::filesystem::path>{
                          action == L"--console" ? argv[2] : argv[1]};
            console_stop_requested = 0;
            std::signal(SIGINT, handle_signal);
            std::signal(SIGTERM, handle_signal);
            run_runtime(path, [] {});
        } else {
            throw std::invalid_argument(
                "usage: lanlink-service [config-path] | --console [config-path] | "
                "--install [config-path] | --start | --stop | --uninstall | --create-shortcut path");
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-service: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
