#include "lanlink/protocol/stream_decoder.hpp"

#include "lanlink/protocol/codec.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace lanlink::protocol {

std::vector<Frame> FrameStreamDecoder::push(const std::span<const std::byte> bytes) {
    constexpr auto maximum_frame_size = frame_header_size + max_payload_size;
    std::vector<Frame> frames;
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const auto buffered = buffer_.size() - read_offset_;
        const auto available = maximum_frame_size - buffered;

        if (available == 0) {
            throw std::runtime_error("protocol frame exceeds the maximum wire size");
        }

        const auto count = std::min(available, bytes.size() - offset);
        buffer_.insert(buffer_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;

        while (read_offset_ < buffer_.size()) {
            const auto unread = std::span<const std::byte>(buffer_).subspan(read_offset_);
            auto result = decode_frame(unread);

            if (result.complete()) {
                frames.push_back(std::move(*result.frame));
                read_offset_ += result.consumed;
                continue;
            }

            if (result.status != DecodeStatus::need_more_data) {
                throw std::runtime_error("invalid protocol stream: " +
                                         std::string(decode_status_name(result.status)));
            }

            break;
        }

        if (read_offset_ == buffer_.size()) {
            buffer_.clear();
            read_offset_ = 0;
        } else if (read_offset_ > buffer_.size() / 2) {
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
            read_offset_ = 0;
        }
    }

    return frames;
}

void FrameStreamDecoder::reset() noexcept {
    buffer_.clear();
    read_offset_ = 0;
}

std::size_t FrameStreamDecoder::buffered_size() const noexcept {
    return buffer_.size() - read_offset_;
}

}
