#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace lanlink::transport {
[[nodiscard]] std::vector<std::byte> fetch_directory(const std::string& url);
}
