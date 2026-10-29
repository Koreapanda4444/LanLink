#include "lanlink/protocol/game_profiles.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    using namespace lanlink::protocol;
    try {
        const auto require = [](bool condition) { if (!condition) throw std::runtime_error("game profile validation failed"); };
        require(game_profiles().size() == 1 && game_profiles()[0].first_port == 8303 && game_profiles()[0].last_port == 8310);
        const auto request = teeworlds_token_request(42);
        require(request.size() == 520 && request[7] == std::byte{5});
        auto response = request; response.resize(12);
        response[3] = response[4] = response[5] = std::byte{0}; response[6] = std::byte{42}; response[11] = std::byte{7};
        require(teeworlds_server_token(response, 42) == 7 && !teeworlds_server_token(response, 43));
        auto info = teeworlds_info_request(7, 42, 3);
        info[13] = std::byte{'i'}; info[14] = std::byte{'n'}; info[15] = std::byte{'f'};
        for (const auto character : std::string("0.7.5\0Fixture\0", 14)) info.push_back(std::byte{static_cast<unsigned char>(character)});
        require(teeworlds_server_info(info, 42, 3).has_value() && !teeworlds_server_info(info, 42, 4));
        info[19] = std::byte{'9'};
        require(!teeworlds_server_info(info, 42, 3));
        std::cout << "game profile tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
