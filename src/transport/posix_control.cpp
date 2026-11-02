#include "lanlink/transport/local_pipe.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <grp.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>
#include <utility>
namespace lanlink::transport {
namespace {
using Clock=std::chrono::steady_clock;
class Descriptor {
    int value_=-1;
public:
    explicit Descriptor(int value=-1):value_(value){}
    ~Descriptor(){if(value_>=0)::close(value_);}
    Descriptor(const Descriptor&)=delete;
    Descriptor& operator=(const Descriptor&)=delete;
    int get() const{return value_;}
};
void checked(int result,const char* operation){if(result<0)throw std::system_error(errno,std::generic_category(),operation);}
std::filesystem::path endpoint() {
    const auto* custom=std::getenv("LANLINK_CONTROL_SOCKET");
    if(custom && *custom)return custom;
    return "/var/run/lanlink/control.sock";
}
uid_t directory_owner(const std::filesystem::path& path) {
    struct stat info{};
    if(!path.is_absolute() || lstat(path.parent_path().c_str(),&info)<0 || !S_ISDIR(info.st_mode) ||
       (info.st_mode & 0022) || (info.st_uid!=0 && info.st_uid!=geteuid()))
        throw std::runtime_error("control socket requires a private directory owned by root or the current user");
    return info.st_uid;
}
sockaddr_un address(const std::filesystem::path& path) {
    sockaddr_un result{};result.sun_family=AF_UNIX;
    const auto name=path.string();
    if(name.size()>=sizeof(result.sun_path))throw std::invalid_argument("control socket path is too long");
    std::memcpy(result.sun_path,name.c_str(),name.size()+1);return result;
}
void prepare(int fd) {
    checked(fcntl(fd,F_SETFD,FD_CLOEXEC),"protect control socket descriptor");
    checked(fcntl(fd,F_SETFL,O_NONBLOCK),"set nonblocking control socket");
#ifdef SO_NOSIGPIPE
    int enabled=1;checked(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)),"disable socket SIGPIPE");
#endif
}
uid_t peer_owner(int fd) {
#ifdef __linux__
    ucred credentials{};socklen_t length=sizeof(credentials);
    checked(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&length),"authenticate local peer");
    if(length!=sizeof(credentials) || credentials.pid<=0)throw std::runtime_error("invalid local peer credentials");
    return credentials.uid;
#else
    uid_t user=0;gid_t group=0;checked(getpeereid(fd,&user,&group),"authenticate local peer");return user;
#endif
}
void transfer(int fd,std::span<std::byte> bytes,bool writing,const std::atomic_bool* stopping,Clock::time_point deadline) {
    std::size_t offset=0;
    while(offset<bytes.size()) {
        if((stopping && stopping->load()) || Clock::now()>=deadline)throw std::runtime_error("local control transfer stopped or timed out");
        pollfd item{fd,static_cast<short>(writing?POLLOUT:POLLIN),0};
        const auto ready=poll(&item,1,100);
        if(ready<0 && errno==EINTR)continue;
        checked(ready,"poll local control");if(!ready)continue;
        if(!(item.revents & (writing?POLLOUT:POLLIN)))throw std::runtime_error("local control disconnected");
        ssize_t count=0;
        if(writing) {
#ifdef MSG_NOSIGNAL
            count=send(fd,bytes.data()+offset,bytes.size()-offset,MSG_NOSIGNAL);
#else
            count=send(fd,bytes.data()+offset,bytes.size()-offset,0);
#endif
        } else count=recv(fd,bytes.data()+offset,bytes.size()-offset,0);
        if(count<0 && (errno==EINTR || errno==EAGAIN))continue;
        checked(static_cast<int>(count),"transfer local control frame");
        if(!count)throw std::runtime_error("incomplete local control frame");
        offset+=static_cast<std::size_t>(count);
    }
}
protocol::LocalMessage receive(int fd,const std::atomic_bool* stopping) {
    const auto deadline=Clock::now()+std::chrono::seconds{15};
    std::vector<std::byte> bytes(protocol::local_control_header_size);
    transfer(fd,bytes,false,stopping,deadline);
    std::uint32_t size=0;for(unsigned i=0;i<4;++i)size|=std::to_integer<std::uint32_t>(bytes[8+i])<<(8*i);
    if(size>protocol::max_local_control_payload)throw std::invalid_argument("oversized local control frame");
    bytes.resize(protocol::local_control_header_size+size);
    transfer(fd,std::span(bytes).subspan(protocol::local_control_header_size),false,stopping,deadline);
    return protocol::decode_local_message(bytes);
}
void send_message(int fd,const protocol::LocalMessage& message,const std::atomic_bool* stopping) {
    auto bytes=protocol::encode_local_message(message);transfer(fd,bytes,true,stopping,Clock::now()+std::chrono::seconds{15});
}
}
class LocalPipeServer::Impl {
    std::filesystem::path path_;
    int listener_=-1,lock_=-1;
    bool bound_=false;
    Handler handler_;
    std::atomic_bool stopping_=false;
    std::thread worker_;
    void cleanup() noexcept {
        if(listener_>=0)::close(listener_);
        listener_=-1;
        if(bound_) {static_cast<void>(unlink(path_.c_str()));bound_=false;}
        if(lock_>=0)::close(lock_);
        lock_=-1;
    }
    void serve() noexcept {
        while(!stopping_) {
            pollfd item{listener_,POLLIN,0};
            const auto ready=poll(&item,1,100);
            if(ready<=0 || !(item.revents&POLLIN))continue;
            Descriptor client(accept(listener_,nullptr,nullptr));if(client.get()<0)continue;
            try {
                prepare(client.get());static_cast<void>(peer_owner(client.get()));
                const auto request=receive(client.get(),&stopping_);
                if(request.response || request.error)throw std::invalid_argument("expected a local control request");
                protocol::LocalMessage response;
                try {
                    response=handler_(request);
                    if(!response.response || response.command!=request.command)throw std::runtime_error("invalid local control response");
                }catch(const std::exception& error){
                    const std::string message=std::string(error.what()).substr(0,4096);
                    response={request.command,true,true,{reinterpret_cast<const std::byte*>(message.data()),reinterpret_cast<const std::byte*>(message.data()+message.size())}};
                }
                send_message(client.get(),response,&stopping_);
            }catch(...){}
        }
    }
public:
    explicit Impl(Handler handler):path_(endpoint()),handler_(std::move(handler)) {
        if(!handler_)throw std::invalid_argument("missing local control handler");
        if(directory_owner(path_)!=geteuid())throw std::runtime_error("service must own its control socket directory");
        try {
            const auto lock_path=path_.string()+".lock";
            lock_=open(lock_path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);checked(lock_,"open control socket lock");
            struct stat info{};checked(fstat(lock_,&info),"inspect control socket lock");
            if(!S_ISREG(info.st_mode) || info.st_uid!=geteuid() || info.st_nlink!=1)throw std::runtime_error("unsafe control socket lock");
            checked(flock(lock_,LOCK_EX|LOCK_NB),"lock control socket");
            if(lstat(path_.c_str(),&info)==0) {
                if(!S_ISSOCK(info.st_mode) || info.st_uid!=geteuid())throw std::runtime_error("refusing to replace an unrelated control path");
                checked(unlink(path_.c_str()),"remove stale control socket");
            }else if(errno!=ENOENT)checked(-1,"inspect control socket");
            listener_=socket(AF_UNIX,SOCK_STREAM,0);checked(listener_,"open control listener");prepare(listener_);
            const auto local=address(path_);
            checked(bind(listener_,reinterpret_cast<const sockaddr*>(&local),sizeof(local)),"bind control socket");bound_=true;
            if(geteuid()==0)if(const auto* group=getgrnam("lanlink"))checked(chown(path_.c_str(),0,group->gr_gid),"set control socket group");
            checked(chmod(path_.c_str(),0660),"protect control socket permissions");
            checked(listen(listener_,8),"listen on control socket");
            worker_=std::thread([this]{serve();});
        }catch(...){cleanup();throw;}
    }
    ~Impl(){stop();}
    void stop() noexcept {stopping_=true;if(worker_.joinable())worker_.join();cleanup();}
};
LocalPipeServer::LocalPipeServer(Handler handler):impl_(std::make_unique<Impl>(std::move(handler))){}
LocalPipeServer::~LocalPipeServer()=default;
void LocalPipeServer::stop() noexcept{impl_->stop();}
protocol::LocalMessage local_pipe_request(const protocol::LocalMessage& request) {
    if(request.response || request.error)throw std::invalid_argument("expected a local control request");
    const auto path=endpoint();const auto owner=directory_owner(path);
    struct stat info{};checked(lstat(path.c_str(),&info),"inspect service socket");
    if(!S_ISSOCK(info.st_mode) || info.st_uid!=owner || (info.st_mode&0007))throw std::runtime_error("untrusted service socket");
    Descriptor connection(socket(AF_UNIX,SOCK_STREAM,0));checked(connection.get(),"open control connection");prepare(connection.get());
    const auto local=address(path);
    const auto result=connect(connection.get(),reinterpret_cast<const sockaddr*>(&local),sizeof(local));
    if(result<0) {
        if(errno!=EINPROGRESS && errno!=EAGAIN)checked(result,"connect local service");
        pollfd item{connection.get(),POLLOUT,0};
        const auto ready=poll(&item,1,3000);checked(ready,"wait for local service");
        if(!ready)throw std::runtime_error("local service connection timed out");
        int error=0;socklen_t size=sizeof(error);checked(getsockopt(connection.get(),SOL_SOCKET,SO_ERROR,&error,&size),"check local service connection");
        if(error)throw std::system_error(error,std::generic_category(),"connect local service");
    }
    if(peer_owner(connection.get())!=owner)throw std::runtime_error("service peer credentials do not match its socket");
    send_message(connection.get(),request,nullptr);const auto response=receive(connection.get(),nullptr);
    if(!response.response || response.command!=request.command)throw std::runtime_error("unexpected local control response");
    if(response.error)throw std::runtime_error(std::string(reinterpret_cast<const char*>(response.payload.data()),response.payload.size()));
    return response;
}
}
