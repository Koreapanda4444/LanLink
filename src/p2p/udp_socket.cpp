#include "lanlink/p2p/udp_socket.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace lanlink::p2p {
namespace {
#ifdef _WIN32
using Socket=SOCKET;
constexpr Socket invalid_socket=INVALID_SOCKET;
void close_socket(Socket s){closesocket(s);}
void initialize(){static const bool ready=[]{WSADATA data{};return WSAStartup(MAKEWORD(2,2),&data)==0;}(); if(!ready)throw std::runtime_error("UDP initialization failed");}
#else
using Socket=int;
constexpr Socket invalid_socket=-1;
void close_socket(Socket s){close(s);}
void initialize(){}
#endif
sockaddr_in address(Endpoint e){sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(e.ipv4);a.sin_port=htons(e.port);return a;}
}
struct UdpSocket::Impl {Socket socket=invalid_socket; ~Impl(){if(socket!=invalid_socket)close_socket(socket);}};
UdpSocket::UdpSocket(Endpoint local):impl_(std::make_unique<Impl>()) {
    initialize(); impl_->socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (impl_->socket==invalid_socket) throw std::runtime_error("UDP socket creation failed");
    auto a=address(local);
    if(bind(impl_->socket,reinterpret_cast<const sockaddr*>(&a),sizeof(a))!=0)throw std::runtime_error("UDP bind failed");
}
UdpSocket::~UdpSocket()=default;
Endpoint UdpSocket::local_endpoint() const {
    sockaddr_in a{};
#ifdef _WIN32
    int size=sizeof(a);
#else
    socklen_t size=sizeof(a);
#endif
    if(getsockname(impl_->socket,reinterpret_cast<sockaddr*>(&a),&size)!=0)throw std::runtime_error("UDP local address unavailable");
    return {ntohl(a.sin_addr.s_addr),ntohs(a.sin_port)};
}
bool UdpSocket::send(Endpoint destination,std::span<const std::byte> bytes) const {
    if(!usable_endpoint(destination) || bytes.empty() || bytes.size()>1200)return false;
    auto a=address(destination);
    const auto sent=sendto(impl_->socket,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<const sockaddr*>(&a),sizeof(a));
    return sent==static_cast<int>(bytes.size());
}
std::optional<Datagram> UdpSocket::receive(std::chrono::milliseconds timeout) const {
    timeout=std::clamp(timeout,std::chrono::milliseconds{0},std::chrono::milliseconds{2000});
    fd_set set; FD_ZERO(&set); FD_SET(impl_->socket,&set);
    timeval duration{};
    duration.tv_sec=static_cast<decltype(duration.tv_sec)>(timeout.count()/1000);
    duration.tv_usec=static_cast<decltype(duration.tv_usec)>((timeout.count()%1000)*1000);
#ifdef _WIN32
    const int nfds=0;
#else
    const int nfds=impl_->socket+1;
#endif
    if(select(nfds,&set,nullptr,nullptr,&duration)<=0)return std::nullopt;
    std::array<std::byte,1201> bytes{};sockaddr_in source{};
#ifdef _WIN32
    int size=sizeof(source);
#else
    socklen_t size=sizeof(source);
#endif
    const auto received=recvfrom(impl_->socket,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<sockaddr*>(&source),&size);
    if(received<=0 || received>1200)return std::nullopt;
    return Datagram{{ntohl(source.sin_addr.s_addr),ntohs(source.sin_port)},{bytes.begin(),bytes.begin()+received}};
}
}
