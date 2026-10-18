#pragma once

#include <filesystem>
#include <memory>

namespace lanlink::transport {

class SchannelCertificate {
public:
    SchannelCertificate(const std::filesystem::path& certificate,
                        const std::filesystem::path& private_key);
    ~SchannelCertificate();
    [[nodiscard]] void* context() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
