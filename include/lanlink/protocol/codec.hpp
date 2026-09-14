#pragma once

#include "lanlink/protocol/message.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lanlink::protocol {

enum class DecodeStatus {
    complete,
    need_more_data,
    invalid_magic,
    unsupported_version,
    unknown_message_type,
    payload_too_large,
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::need_more_data;
    std::optional<Frame> frame;
    std::size_t consumed = 0;
    std::size_t required = frame_header_size;

    [[nodiscard]] bool complete() const noexcept {
        return status == DecodeStatus::complete;
    }
};

[[nodiscard]] std::vector<std::byte> encode_frame(const Frame& frame);
[[nodiscard]] DecodeResult decode_frame(std::span<const std::byte> bytes);
[[nodiscard]] std::string_view decode_status_name(DecodeStatus status) noexcept;

}
