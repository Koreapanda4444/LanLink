#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <cstddef>

namespace lanlink::transport {

class SchannelCertificate {
public:
    SchannelCertificate(const std::filesystem::path& certificate,
                        const std::filesystem::path& private_key);
    explicit SchannelCertificate(std::span<const std::byte> pkcs12);
    ~SchannelCertificate();
    [[nodiscard]] void* context() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
