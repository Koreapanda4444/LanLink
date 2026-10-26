#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lanlink::protocol {
class BinaryWriter {
public:
    template<class T> void put(T value) {
        static_assert(std::is_unsigned_v<T>);
        for (std::size_t i = sizeof(T); i > 0; --i) data_.push_back(static_cast<std::byte>((value >> ((i - 1) * 8)) & 255));
    }
    void append(std::span<const std::byte> bytes) { data_.insert(data_.end(), bytes.begin(), bytes.end()); }
    [[nodiscard]] std::vector<std::byte> finish() && { return std::move(data_); }
private:
    std::vector<std::byte> data_;
};
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> value) : data_(value) {}
    template<class T> [[nodiscard]] T get() {
        static_assert(std::is_unsigned_v<T>);
        const auto value = bytes(sizeof(T)); T result = 0;
        for (auto byte : value) result = static_cast<T>((result << 8) | std::to_integer<unsigned>(byte));
        return result;
    }
    [[nodiscard]] std::span<const std::byte> bytes(std::size_t count) {
        if (count > data_.size()) throw std::invalid_argument("truncated protocol payload");
        const auto result = data_.first(count); data_ = data_.subspan(count); return result;
    }
    template<std::size_t N> [[nodiscard]] std::array<std::byte, N> array() {
        std::array<std::byte, N> result{}; const auto value = bytes(N);
        std::copy(value.begin(), value.end(), result.begin()); return result;
    }
    void finish() const { if (!data_.empty()) throw std::invalid_argument("trailing protocol payload"); }
private:
    std::span<const std::byte> data_;
};
}
