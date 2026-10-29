#include "lanlink/transport/directory_fetch.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <sys/select.h>
#endif

namespace lanlink::transport {
namespace {
struct Url { std::string host; std::string port; std::string path; };
Url parse(const std::string& url) {
    if (!url.starts_with("https://") || url.size()>2048 || url.find_first_of("\r\n\t @#\\")!=url.npos)
        throw std::invalid_argument("directory requires an HTTPS URL without credentials");
    const auto slash=url.find('/',8); auto authority=url.substr(8,slash==url.npos ? url.size()-8 : slash-8);
    const auto colon=authority.find(':'); Url out{authority.substr(0,colon),"443",slash==url.npos ? "/" : url.substr(slash)};
    if(colon!=authority.npos) out.port=authority.substr(colon+1);
    if(out.host.empty() || out.port.empty() || out.port.size()>5 || !std::all_of(out.port.begin(),out.port.end(),[](char c){return c>='0' && c<='9';}) || std::stoul(out.port)==0 || std::stoul(out.port)>65535)
        throw std::invalid_argument("invalid directory URL authority");
    return out;
}
}
std::vector<std::byte> fetch_directory(const std::string& url) {
    const auto target=parse(url);
#ifdef _WIN32
    auto wide=[](const std::string& x){ return std::wstring(x.begin(),x.end()); };
    using Handle=std::unique_ptr<void,decltype(&WinHttpCloseHandle)>;
    Handle session(WinHttpOpen(L"LanLink/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0),WinHttpCloseHandle);
    if(!session || !WinHttpSetTimeouts(session.get(),5000,5000,5000,5000)) throw std::runtime_error("directory HTTP initialization failed");
    Handle connection(WinHttpConnect(session.get(),wide(target.host).c_str(),static_cast<INTERNET_PORT>(std::stoul(target.port)),0),WinHttpCloseHandle);
    if(!connection) throw std::runtime_error("directory connect failed");
    Handle request(WinHttpOpenRequest(connection.get(),L"GET",wide(target.path).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE),WinHttpCloseHandle);
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!request || !WinHttpSetOption(request.get(),WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)) ||
       !WinHttpSendRequest(request.get(),WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request.get(),nullptr))
        throw std::runtime_error("directory HTTPS request failed");
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) || status!=200)
        throw std::runtime_error("directory HTTP status rejected");
    std::vector<std::byte> data; std::array<std::byte,4096> buffer{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;) {
        DWORD read=0;
        if(std::chrono::steady_clock::now()>deadline || !WinHttpReadData(request.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&read)) throw std::runtime_error("directory read failed");
        if(!read) return data;
        if(data.size()+read>16384) throw std::runtime_error("directory response too large");
        data.insert(data.end(),buffer.begin(),buffer.begin()+read);
    }
#else
    using Context=std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)>;
    using Bio=std::unique_ptr<BIO,decltype(&BIO_free_all)>;
    Context context(SSL_CTX_new(TLS_client_method()),SSL_CTX_free);
    if(!context || SSL_CTX_set_default_verify_paths(context.get())!=1) throw std::runtime_error("directory trust store unavailable");
    SSL_CTX_set_verify(context.get(),SSL_VERIFY_PEER,nullptr);
    SSL_CTX_set_min_proto_version(context.get(),TLS1_2_VERSION);
    Bio bio(BIO_new_ssl_connect(context.get()),BIO_free_all);
    if(!bio) throw std::runtime_error("directory TLS initialization failed");
    SSL* ssl=nullptr; BIO_get_ssl(bio.get(),&ssl);
    std::array<unsigned char,16> address{};
    const bool numeric=inet_pton(AF_INET,target.host.c_str(),address.data())==1;
    if(!ssl || SSL_set_tlsext_host_name(ssl,target.host.c_str())!=1 ||
       (numeric ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl),target.host.c_str()) : SSL_set1_host(ssl,target.host.c_str()))!=1)
        throw std::runtime_error("directory hostname setup failed");
    const auto endpoint=target.host+":"+target.port;
    BIO_set_conn_hostname(bio.get(),endpoint.c_str()); BIO_set_nbio(bio.get(),1);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    auto retry=[&] {
        if(!BIO_should_retry(bio.get()) || std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("directory HTTPS operation failed");
        int fd=-1; BIO_get_fd(bio.get(),&fd);
        if(fd<0 || fd>=FD_SETSIZE) throw std::runtime_error("directory socket unavailable");
        fd_set read,write; FD_ZERO(&read); FD_ZERO(&write);
        if(BIO_should_read(bio.get())) FD_SET(fd,&read); else FD_SET(fd,&write);
        timeval wait{0,100000}; static_cast<void>(select(fd+1,&read,&write,nullptr,&wait));
    };
    while(BIO_do_connect(bio.get())<=0) retry();
    const auto request="GET "+target.path+" HTTP/1.0\r\nHost: "+target.host+"\r\nConnection: close\r\n\r\n";
    std::size_t offset=0;
    while(offset<request.size()) {
        const auto sent=BIO_write(bio.get(),request.data()+offset,static_cast<int>(request.size()-offset));
        if(sent<=0) retry(); else offset+=static_cast<std::size_t>(sent);
    }
    std::string response; std::array<char,4096> buffer{};
    for(;;) {
        const auto n=BIO_read(bio.get(),buffer.data(),static_cast<int>(buffer.size()));
        if(n>0) response.append(buffer.data(),static_cast<std::size_t>(n));
        else if(BIO_should_retry(bio.get())) retry(); else if(n==0) break; else throw std::runtime_error("directory response truncated");
        if(response.size()>24576 || std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("directory response limit exceeded");
    }
    const auto split=response.find("\r\n\r\n");
    if((!response.starts_with("HTTP/1.0 200 ") && !response.starts_with("HTTP/1.1 200 ")) || split==response.npos || split>8192 || response.size()-split-4>16384)
        throw std::runtime_error("directory HTTP response rejected");
    const auto bytes=std::as_bytes(std::span(response.data()+split+4,response.size()-split-4));
    return {bytes.begin(),bytes.end()};
#endif
}
}
