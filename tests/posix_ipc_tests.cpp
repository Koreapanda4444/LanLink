#include "lanlink/transport/local_pipe.hpp"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lanlink;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){
    const auto root=std::filesystem::temp_directory_path()/("lanlink-ipc-"+std::to_string(getpid()));
    try{
        std::filesystem::create_directory(root);chmod(root.c_str(),0700);
        const auto path=root/"control.sock";setenv("LANLINK_CONTROL_SOCKET",path.c_str(),1);
        const auto handler=[](const protocol::LocalMessage& request){
            if(request.command!=protocol::LocalCommand::status)throw std::runtime_error("denied fixture command");
            return protocol::LocalMessage{request.command,true,false,std::vector<std::byte>(262144,std::byte{42})};
        };
        {
            transport::LocalPipeServer server(handler);
            bool duplicate=false;try{transport::LocalPipeServer other(handler);}catch(const std::exception&){duplicate=true;}
            require(duplicate,"duplicate server stole the control socket");
            const auto reply=transport::local_pipe_request({protocol::LocalCommand::status,false,false,{}});
            require(reply.payload.size()==262144 && reply.payload.back()==std::byte{42},"stream framing truncated a large reply");
            bool denied=false;try{static_cast<void>(transport::local_pipe_request({protocol::LocalCommand::stop,false,false,{}}));}catch(const std::exception&){denied=true;}
            require(denied,"handler error was lost");
            struct stat info{};require(lstat(path.c_str(),&info)==0 && (info.st_mode&0777)==0660,"unsafe socket permissions");
            const auto fd=socket(AF_UNIX,SOCK_STREAM,0);sockaddr_un local{};local.sun_family=AF_UNIX;std::strcpy(local.sun_path,path.c_str());
            require(connect(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0,"slow client connect failed");
            const char byte='L';send(fd,&byte,1,0);std::this_thread::sleep_for(std::chrono::milliseconds{30});
            const auto start=std::chrono::steady_clock::now();server.stop();
            require(std::chrono::steady_clock::now()-start<std::chrono::seconds{1},"slow client blocked shutdown");close(fd);
        }
        require(!std::filesystem::exists(path),"control socket survived shutdown");
        std::ofstream(path)<<"unrelated file";bool protected_file=false;
        try{transport::LocalPipeServer server(handler);}catch(const std::exception&){protected_file=true;}
        require(protected_file && std::filesystem::is_regular_file(path),"server removed an unrelated file");std::filesystem::remove(path);
        chmod(root.c_str(),0777);bool protected_directory=false;
        try{transport::LocalPipeServer server(handler);}catch(const std::exception&){protected_directory=true;}
        require(protected_directory,"server accepted a writable control directory");
        std::filesystem::remove_all(root);std::cout<<"Unix IPC: permissions, peer credentials, framing, errors, singleton and bounded shutdown passed\n";return 0;
    }catch(const std::exception& error){std::filesystem::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
