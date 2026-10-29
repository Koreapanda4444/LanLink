#include "lanlink/auth/directory.hpp"
#include "lanlink/transport/directory_fetch.hpp"
#include <chrono>
#include <iostream>
int main(int argc,char* argv[]) {
    try {
        if(argc!=3) return 2;
        const auto bytes=lanlink::transport::fetch_directory(argv[1]);
        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const auto c=lanlink::auth::verify_directory(bytes,lanlink::auth::parse_directory_key(argv[2]),now);
        std::cout<<c.relays.front().host<<'\n'; return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
