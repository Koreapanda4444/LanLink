#pragma once

#include "lanlink/protocol/message.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace lanlink::protocol {

class FrameStreamDecoder {
public:
    [[nodiscard]] std::vector<Frame> push(std::span<const std::byte> bytes);
    void reset() noexcept;
    [[nodiscard]] std::size_t buffered_size() const noexcept;

private:
    std::vector<std::byte> buffer_;
    std::size_t read_offset_ = 0;
};

}
