#include "lanlink/auth/directory.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char* argv[]) {
    try {
        if(argc<6 || (argc-3)%3!=0) throw std::invalid_argument("usage: lanlink-directory-sign <identity-file> <output> <id> <host> <port> [id host port...]");
        auto identity=lanlink::auth::DeviceIdentity::load_or_create(argv[1]);
        lanlink::auth::DirectoryCatalog catalog;
        catalog.issued_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        catalog.expires_ms=catalog.issued_ms+3600000;
        for(int i=3;i<argc;i+=3) {
            const std::string port=argv[i+2]; std::size_t end=0; const auto value=std::stoul(port,&end);
            if(end!=port.size() || !value || value>65535) throw std::invalid_argument("invalid relay port");
            catalog.relays.push_back({argv[i],argv[i+1],static_cast<std::uint16_t>(value)});
        }
        const auto bytes=lanlink::auth::sign_directory(catalog,identity);
        std::ofstream out(argv[2],std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!out) throw std::runtime_error("cannot write directory catalog");
        std::cout << "directory_public_key=" << lanlink::auth::hex_encode(identity.public_key()) << '\n'; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
