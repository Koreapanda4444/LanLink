#include "lanlink/platform/service_runtime.hpp"
#include "lanlink/core/component.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
volatile std::sig_atomic_t signal_stop=0;
std::atomic_bool requested_stop=false;
void stop(int){signal_stop=1;}
}
int main(int argc,char* argv[]) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--version") {std::cout<<lanlink::core::project_version()<<'\n';return 0;}
        if(argc!=3 || std::string_view(argv[1])!="--console")throw std::invalid_argument("usage: lanlink-service --console /absolute/client.conf | --version");
        const std::filesystem::path path=argv[2];if(!path.is_absolute())throw std::invalid_argument("service configuration must be an absolute path");
        umask(0077);std::signal(SIGTERM,stop);std::signal(SIGINT,stop);std::signal(SIGPIPE,SIG_IGN);
        if(!std::getenv("LANLINK_CONTROL_SOCKET")) {
            if(geteuid()!=0)throw std::runtime_error("start the installed system service or run the console as root");
            if(mkdir("/var/run/lanlink",0755)<0 && errno!=EEXIST)throw std::runtime_error("cannot create service runtime directory");
        }
        lanlink::service::run_client(path,{[]{std::cout<<"LanLink service ready\n"<<std::flush;},
            []{return signal_stop!=0 || requested_stop.load();},[]{requested_stop=true;}});
        return 0;
    }catch(const std::exception& error){std::cerr<<"lanlink-service: "<<error.what()<<'\n';return 1;}
}
