#include "lanlink/auth/directory.hpp"
#include "lanlink/core/component.hpp"
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
std::uint64_t number(std::string_view value,std::uint64_t maximum) {
    std::uint64_t result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !result || result>maximum)
        throw std::invalid_argument("invalid directory number");
    return result;
}
}
int main(int argc,char* argv[]) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--version") {
            std::cout<<lanlink::core::project_version()<<'\n';return 0;
        }
        if(argc==4 && std::string_view(argv[1])=="--verify") {
            std::ifstream input(argv[3],std::ios::binary);
            if(!input)throw std::runtime_error("cannot open directory catalog");
            std::vector<std::byte> data(16385);
            input.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
            data.resize(static_cast<std::size_t>(input.gcount()));
            if(!input.eof() || input.bad() || data.size()>16384)throw std::runtime_error("directory catalog exceeds 16 KiB or cannot be read");
            const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            const auto verified=lanlink::auth::verify_directory(data,lanlink::auth::parse_directory_key(argv[2]),now);
            std::cout<<"verified revision="<<verified.revision<<" expires_ms="<<verified.expires_ms<<'\n';return 0;
        }
        const bool regional=argc>1 && std::string_view(argv[1])=="--regional";
        const int begin=regional?6:3, width=regional?7:3;
        if(argc<begin+width || (argc-begin)%width!=0)throw std::invalid_argument(
            "usage: lanlink-directory-sign [--regional official|self_hosted REVISION] IDENTITY OUTPUT ID HOST PORT [REGION CAPACITY AUTHORITY HEALTH_PUBLIC_KEY] ...");
        auto identity=lanlink::auth::DeviceIdentity::load_or_create(argv[regional?4:1]);
        lanlink::auth::DirectoryCatalog catalog;
        if(regional) {
            const std::string_view scope=argv[2];
            if(scope!="official" && scope!="self_hosted")throw std::invalid_argument("invalid directory scope");
            catalog.scope=scope=="official"?lanlink::auth::DirectoryScope::official:lanlink::auth::DirectoryScope::self_hosted;
            catalog.revision=number(argv[3],UINT64_MAX);
        }
        catalog.issued_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        catalog.expires_ms=catalog.issued_ms+3600000;
        for(int i=begin;i<argc;i+=width) {
            lanlink::auth::RelayEndpoint relay;relay.id=argv[i];relay.host=argv[i+1];relay.port=static_cast<std::uint16_t>(number(argv[i+2],65535));
            if(regional) {
                relay.region=argv[i+3];relay.capacity=static_cast<std::uint32_t>(number(argv[i+4],1'000'000));
                relay.authority=argv[i+5];relay.health_key=lanlink::auth::parse_directory_key(argv[i+6]);
            }
            catalog.relays.push_back(std::move(relay));
        }
        const auto bytes=lanlink::auth::sign_directory(catalog,identity);
        std::ofstream out(argv[regional?5:2],std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!out)throw std::runtime_error("cannot write directory catalog");
        std::cout<<"directory_public_key="<<lanlink::auth::hex_encode(identity.public_key())<<'\n';return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
