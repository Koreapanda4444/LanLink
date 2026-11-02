#pragma once
#include <filesystem>
#include <functional>
#include <optional>
namespace lanlink::service {
struct ServiceHooks {
    std::function<void()> ready;
    std::function<bool()> stop_requested;
    std::function<void()> request_stop;
};
void run_client(const std::optional<std::filesystem::path>& configuration, const ServiceHooks& hooks);
}
