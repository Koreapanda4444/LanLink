#include "lanlink/protocol/ipv4.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace lanlink::protocol;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void word(std::vector<std::byte>& bytes, std::size_t offset, std::size_t value) {
    bytes[offset] = std::byte{static_cast<unsigned char>(value >> 8U)};
    bytes[offset + 1] = std::byte{static_cast<unsigned char>(value)};
}
std::vector<std::byte> packet(std::size_t size) {
    std::vector<std::byte> bytes(size);
    bytes[0] = std::byte{0x45}; bytes[8] = std::byte{64}; bytes[9] = std::byte{17};
    word(bytes, 2, size); word(bytes, 4, 0x1234);
    bytes[12] = bytes[16] = std::byte{10}; bytes[13] = bytes[17] = std::byte{64};
    bytes[15] = std::byte{1}; bytes[19] = std::byte{2};
    for (std::size_t index = 20; index < size; ++index) bytes[index] = std::byte{static_cast<unsigned char>(index % 251)};
    update_ipv4_checksum(bytes);
    return bytes;
}
template<class Function> void rejects(Function function) {
    bool failed = false; try { function(); } catch (const std::exception&) { failed = true; }
    require(failed, "invalid IPv4 input was accepted");
}
void scenario() {
    const auto original = packet(8192);
    auto parts = fragment_ipv4(original, 576);
    std::vector<std::byte> payload;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& part = parts[index]; const auto header = inspect_ipv4(part);
        require(part.size() <= 576 && header.fragment_offset == payload.size() &&
                header.more_fragments == (index + 1 < parts.size()) && part[4] == original[4] && part[5] == original[5],
                "fragment size, ID, offset or MF flag changed");
        payload.insert(payload.end(), part.begin() + static_cast<std::ptrdiff_t>(header.header_size), part.end());
    }
    require(std::equal(payload.begin(), payload.end(), original.begin() + 20), "fragmentation changed application bytes");
    const auto refined = fragment_ipv4(fragment_ipv4(original, 1400)[1], 576);
    require(inspect_ipv4(refined.front()).fragment_offset == 1376 && inspect_ipv4(refined.back()).more_fragments,
            "refragmentation lost the original offset or MF flag");
    auto df = original; df[6] = std::byte{0x40}; update_ipv4_checksum(df);
    rejects([&] { static_cast<void>(fragment_ipv4(df, 576)); });
    const auto error = ipv4_fragmentation_needed(df, 0x0a400002U, 576);
    require(error && inspect_ipv4(*error).destination == 0x0a400001U && (*error)[20] == std::byte{3} && (*error)[21] == std::byte{4} &&
            (*error)[26] == std::byte{2} && (*error)[27] == std::byte{64} &&
            internet_checksum(std::span<const std::byte>(*error).subspan(20)) == 0,
            "fragmentation-needed ICMP has an invalid checksum, destination or MTU");
    auto options = packet(3000); options.insert(options.begin() + 20,
        {std::byte{0x1e}, std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0x9e}, std::byte{4}, std::byte{0}, std::byte{0}});
    options[0] = std::byte{0x47}; word(options, 2, options.size()); update_ipv4_checksum(options);
    const auto with_options = fragment_ipv4(options, 576);
    require(inspect_ipv4(with_options[0]).header_size == 28 && inspect_ipv4(with_options[1]).header_size == 24 &&
            with_options[1][20] == std::byte{0x9e}, "IPv4 option copy bit was ignored");
    auto bad = original; bad[8] ^= std::byte{1}; rejects([&] { static_cast<void>(inspect_ipv4(bad)); });
    bad = packet(29); bad[6] = std::byte{0x20}; update_ipv4_checksum(bad);
    rejects([&] { static_cast<void>(inspect_ipv4(bad)); });
    bad = original; word(bad, 6, 0x1fff); update_ipv4_checksum(bad);
    rejects([&] { static_cast<void>(inspect_ipv4(bad)); });
    bad = original; word(bad, 6, 0x6000); update_ipv4_checksum(bad);
    rejects([&] { static_cast<void>(inspect_ipv4(bad)); });
    rejects([&] { static_cast<void>(fragment_ipv4(original, 67)); });
}
}
int main() {
    try { scenario(); std::cout << "IPv4 MTU and fragment tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
