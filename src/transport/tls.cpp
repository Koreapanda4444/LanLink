#include "lanlink/transport/tls.hpp"

#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/stream_decoder.hpp"

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lanlink::transport {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(const Socket socket) noexcept { closesocket(socket); }
void stop_socket(const Socket socket) noexcept { shutdown(socket, SD_BOTH); }
void prepare_sockets() {
    static const bool initialized = [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("TCP socket initialization failed");
        }
        return true;
    }();
    static_cast<void>(initialized);
}
bool would_block() noexcept {
    const auto error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
void set_nonblocking(const Socket socket) {
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) != 0) {
        throw std::runtime_error("cannot configure TCP socket");
    }
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(const Socket socket) noexcept { close(socket); }
void stop_socket(const Socket socket) noexcept { shutdown(socket, SHUT_RDWR); }
void prepare_sockets() {}
bool would_block() noexcept { return errno == EINPROGRESS || errno == EWOULDBLOCK; }
void set_nonblocking(const Socket socket) {
    const auto flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("cannot configure TCP socket");
    }
}
#endif

class SocketOwner {
public:
    explicit SocketOwner(const Socket socket = invalid_socket) noexcept : socket_(socket) {}
    ~SocketOwner() { reset(); }
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
    SocketOwner(SocketOwner&& other) noexcept : socket_(std::exchange(other.socket_, invalid_socket)) {}
    SocketOwner& operator=(SocketOwner&& other) noexcept {
        if (this != &other) {
            reset();
            socket_ = std::exchange(other.socket_, invalid_socket);
        }
        return *this;
    }
    [[nodiscard]] Socket get() const noexcept { return socket_; }
    void reset() noexcept {
        if (socket_ != invalid_socket) {
            close_socket(std::exchange(socket_, invalid_socket));
        }
    }
private:
    Socket socket_;
};

bool wait_socket(const Socket socket, const bool read, const bool write) {
    fd_set readers;
    fd_set writers;
    FD_ZERO(&readers);
    FD_ZERO(&writers);
    if (read) {
        FD_SET(socket, &readers);
    }
    if (write) {
        FD_SET(socket, &writers);
    }
    timeval delay{0, 50'000};
    const auto result = select(static_cast<int>(socket) + 1,
                               read ? &readers : nullptr,
                               write ? &writers : nullptr, nullptr, &delay);
    if (result < 0) {
        throw std::runtime_error("TCP socket wait failed");
    }
    return result != 0;
}

SocketOwner connect_socket(const std::string& host, const std::uint16_t port,
                           const std::chrono::steady_clock::time_point deadline,
                           const std::atomic_bool& stopping) {
    prepare_sockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0) {
        throw std::runtime_error("TCP relay address resolution failed");
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(addresses, freeaddrinfo);
    for (auto* address = addresses; address != nullptr && !stopping.load();
         address = address->ai_next) {
        SocketOwner socket(::socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol));
        if (socket.get() == invalid_socket) {
            continue;
        }
        set_nonblocking(socket.get());
        if (::connect(socket.get(), address->ai_addr,
                      static_cast<int>(address->ai_addrlen)) == 0) {
            return socket;
        }
        if (!would_block()) {
            continue;
        }
        while (!stopping.load() && std::chrono::steady_clock::now() < deadline) {
            if (!wait_socket(socket.get(), false, true)) {
                continue;
            }
            int error = 0;
#ifdef _WIN32
            int length = sizeof(error);
#else
            socklen_t length = sizeof(error);
#endif
            if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&error), &length) == 0 && error == 0) {
                return socket;
            }
            break;
        }
    }
    throw std::runtime_error("TLS TCP relay connection failed");
}

SocketOwner listen_socket(const std::string& host, const std::uint16_t port) {
    prepare_sockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const auto port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0) {
        throw std::runtime_error("TCP listen address resolution failed");
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(addresses, freeaddrinfo);
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        SocketOwner socket(::socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol));
        if (socket.get() == invalid_socket) {
            continue;
        }
#ifndef _WIN32
        int reuse = 1;
        static_cast<void>(setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR,
                                      &reuse, sizeof(reuse)));
#endif
        if (::bind(socket.get(), address->ai_addr,
                   static_cast<int>(address->ai_addrlen)) == 0 &&
            ::listen(socket.get(), 128) == 0) {
            set_nonblocking(socket.get());
            return socket;
        }
    }
    throw std::runtime_error("TLS TCP relay listen failed");
}

std::shared_ptr<SSL_CTX> make_context(const bool server) {
    auto context = std::shared_ptr<SSL_CTX>(
        SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION) != 1) {
        throw std::runtime_error("TLS 1.3 context initialization failed");
    }
    if (!server) {
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(context.get()) != 1) {
            throw std::runtime_error("TLS trust store initialization failed");
        }
    } else {
        SSL_CTX_set_alpn_select_cb(context.get(),
            [](SSL*, const unsigned char** output, unsigned char* output_length,
               const unsigned char* input, unsigned int length, void*) -> int {
                auto offset = 0U;
                while (offset < length) {
                    const auto size = input[offset++];
                    if (size > length - offset) {
                        return SSL_TLSEXT_ERR_ALERT_FATAL;
                    }
                    if (size == protocol::protocol_alpn.size() &&
                        std::memcmp(input + offset, protocol::protocol_alpn.data(), size) == 0) {
                        *output = input + offset;
                        *output_length = size;
                        return SSL_TLSEXT_ERR_OK;
                    }
                    offset += size;
                }
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }, nullptr);
    }
    return context;
}

}

class TlsChannel::Impl {
public:
    Impl(SocketOwner socket, std::shared_ptr<SSL_CTX> context)
        : socket_(std::move(socket)), context_(std::move(context)),
          ssl_(SSL_new(context_.get()), SSL_free) {
        if (!ssl_ || SSL_set_fd(ssl_.get(), static_cast<int>(socket_.get())) != 1) {
            throw std::runtime_error("TLS socket initialization failed");
        }
    }

    void configure_client(const std::string& host) {
        auto* parameters = SSL_get0_param(ssl_.get());
        if (X509_VERIFY_PARAM_set1_ip_asc(parameters, host.c_str()) != 1) {
            if (SSL_set1_host(ssl_.get(), host.c_str()) != 1 ||
                SSL_set_tlsext_host_name(ssl_.get(), host.c_str()) != 1) {
                throw std::runtime_error("TLS relay name verification setup failed");
            }
        }
        std::array<unsigned char, 256> alpn{};
        const auto size = protocol::protocol_alpn.size();
        alpn.front() = static_cast<unsigned char>(size);
        std::memcpy(alpn.data() + 1, protocol::protocol_alpn.data(), size);
        if (SSL_set_alpn_protos(ssl_.get(), alpn.data(), static_cast<unsigned int>(size + 1)) != 0) {
            throw std::runtime_error("TLS relay ALPN setup failed");
        }
    }

    void handshake(const bool server, const std::chrono::milliseconds timeout,
                   const std::atomic_bool& external_stop) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!stopping_.load() && !external_stop.load() &&
               std::chrono::steady_clock::now() < deadline) {
            const auto result = server ? SSL_accept(ssl_.get()) : SSL_connect(ssl_.get());
            if (result == 1) {
                const unsigned char* selected = nullptr;
                unsigned int size = 0;
                SSL_get0_alpn_selected(ssl_.get(), &selected, &size);
                if (size != protocol::protocol_alpn.size() ||
                    std::memcmp(selected, protocol::protocol_alpn.data(), size) != 0) {
                    throw std::runtime_error("TLS relay protocol mismatch");
                }
                return;
            }
            const auto error = SSL_get_error(ssl_.get(), result);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                throw std::runtime_error("TLS relay handshake failed");
            }
            static_cast<void>(wait_socket(socket_.get(), error == SSL_ERROR_WANT_READ,
                                          error == SSL_ERROR_WANT_WRITE));
        }
        throw std::runtime_error("TLS relay handshake timed out");
    }

    bool enqueue(const protocol::Frame& frame) noexcept {
        try {
            auto encoded = protocol::encode_frame(frame);
            std::lock_guard lock(queue_mutex_);
            if (stopping_.load() || queue_size_ + encoded.size() > 4U * 1024U * 1024U) {
                return false;
            }
            queue_.push_back(std::move(encoded));
            queue_size_ += queue_.back().size();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool pending_output() const noexcept {
        std::lock_guard lock(queue_mutex_);
        return queue_size_ != 0;
    }

    void run(const FrameHandler& on_frame, const std::function<bool()>& keep_running) {
        protocol::FrameStreamDecoder decoder;
        std::vector<std::byte> writing;
        std::size_t offset = 0;
        std::array<std::byte, 16'384> received{};
        try {
            while (!stopping_.load() && keep_running()) {
                if (writing.empty()) {
                    std::lock_guard lock(queue_mutex_);
                    if (!queue_.empty()) {
                        writing = std::move(queue_.front());
                        queue_.pop_front();
                    }
                }
                if (!writing.empty()) {
                    std::size_t transferred = 0;
                    const auto result = SSL_write_ex(ssl_.get(), writing.data() + offset,
                                                      writing.size() - offset, &transferred);
                    if (result == 1) {
                        sent_bytes_.fetch_add(transferred);
                        offset += transferred;
                        if (offset == writing.size()) {
                            std::lock_guard lock(queue_mutex_);
                            queue_size_ -= writing.size();
                            writing.clear();
                            offset = 0;
                        }
                        continue;
                    }
                    const auto error = SSL_get_error(ssl_.get(), result);
                    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                        break;
                    }
                    static_cast<void>(wait_socket(socket_.get(), error == SSL_ERROR_WANT_READ,
                                                  error == SSL_ERROR_WANT_WRITE));
                    continue;
                }
                std::size_t transferred = 0;
                const auto result = SSL_read_ex(ssl_.get(), received.data(),
                                                 received.size(), &transferred);
                if (result == 1) {
                    received_bytes_.fetch_add(transferred);
                    for (const auto& frame : decoder.push(
                             std::span<const std::byte>(received.data(), transferred))) {
                        on_frame(frame);
                    }
                    continue;
                }
                const auto error = SSL_get_error(ssl_.get(), result);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                    break;
                }
                static_cast<void>(wait_socket(socket_.get(), error == SSL_ERROR_WANT_READ,
                                              error == SSL_ERROR_WANT_WRITE));
            }
        } catch (...) {
        }
        finished_.store(true);
    }

    void stop() noexcept {
        if (!stopping_.exchange(true)) {
            stop_socket(socket_.get());
        }
    }

    SocketOwner socket_;
    std::shared_ptr<SSL_CTX> context_;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_;
    mutable std::mutex queue_mutex_;
    std::deque<std::vector<std::byte>> queue_;
    std::size_t queue_size_ = 0;
    std::atomic_bool stopping_ = false;
    std::atomic_bool finished_ = false;
    std::atomic<std::uint64_t> sent_bytes_ = 0;
    std::atomic<std::uint64_t> received_bytes_ = 0;

    [[nodiscard]] TlsTrafficCounters traffic() const noexcept {
        return {sent_bytes_.load(), received_bytes_.load()};
    }
};

TlsChannel::TlsChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsChannel::~TlsChannel() { stop(); }
bool TlsChannel::enqueue(const protocol::Frame& frame) noexcept {
    return impl_->enqueue(frame);
}
bool TlsChannel::pending_output() const noexcept { return impl_->pending_output(); }
void TlsChannel::run(const FrameHandler& on_frame,
                     const std::function<bool()>& keep_running) {
    impl_->run(on_frame, keep_running);
}
void TlsChannel::stop() noexcept { impl_->stop(); }
bool TlsChannel::finished() const noexcept { return impl_->finished_.load(); }
TlsTrafficCounters TlsChannel::traffic() const noexcept { return impl_->traffic(); }

std::shared_ptr<TlsChannel> TlsChannel::connect(
    const std::string& host, const std::uint16_t port,
    const std::chrono::milliseconds timeout, const std::atomic_bool& stopping) {
    const auto context = make_context(false);
    auto socket = connect_socket(host, port, std::chrono::steady_clock::now() + timeout,
                                 stopping);
    auto channel = std::shared_ptr<TlsChannel>(
        new TlsChannel(std::make_unique<Impl>(std::move(socket), context)));
    channel->impl_->configure_client(host);
    channel->impl_->handshake(false, timeout, stopping);
    return channel;
}

class TlsListener::Impl {
public:
    Impl(std::string host, const std::uint16_t port,
         const std::filesystem::path& certificate,
         const std::filesystem::path& private_key,
         const std::chrono::milliseconds timeout, HandlerFactory factory)
        : host_(std::move(host)), port_(port), timeout_(timeout),
          factory_(std::move(factory)), context_(make_context(true)) {
        sessions_.reserve(128);
        if (SSL_CTX_use_certificate_chain_file(context_.get(),
                                               certificate.string().c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(context_.get(), private_key.string().c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context_.get()) != 1) {
            throw std::runtime_error("TLS TCP relay certificate loading failed");
        }
    }

    ~Impl() { stop(); }

    void start() {
        socket_ = listen_socket(host_, port_);
        stopping_.store(false);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    void stop() noexcept {
        stopping_.store(true);
        if (socket_.get() != invalid_socket) {
            stop_socket(socket_.get());
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        socket_.reset();
        for (auto& session : sessions_) {
            session.channel->stop();
        }
        for (auto& session : sessions_) {
            if (session.worker.joinable()) {
                session.worker.join();
            }
        }
        sessions_.clear();
    }

    void accept_loop() noexcept {
        while (!stopping_.load()) {
            try {
                for (auto it = sessions_.begin(); it != sessions_.end();) {
                    if (it->channel->finished()) {
                        it->worker.join();
                        it = sessions_.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (sessions_.size() >= 128 ||
                    !wait_socket(socket_.get(), true, false)) {
                    continue;
                }
                SocketOwner socket(::accept(socket_.get(), nullptr, nullptr));
                if (socket.get() == invalid_socket) {
                    continue;
                }
                set_nonblocking(socket.get());
                auto channel = std::shared_ptr<TlsChannel>(
                    new TlsChannel(std::make_unique<TlsChannel::Impl>(
                        std::move(socket), context_)));
                std::thread worker([this, channel] {
                    try {
                        channel->impl_->handshake(true, timeout_, stopping_);
                        auto handler = factory_(channel);
                        channel->run(handler.on_frame, handler.keep_running);
                        if (handler.on_close) {
                            handler.on_close();
                        }
                    } catch (...) {
                    }
                    channel->stop();
                    channel->impl_->finished_.store(true);
                });
                sessions_.push_back({std::move(channel), std::move(worker)});
            } catch (...) {
                if (!stopping_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
            }
        }
    }

private:
    struct Session {
        std::shared_ptr<TlsChannel> channel;
        std::thread worker;
    };
    std::string host_;
    std::uint16_t port_;
    std::chrono::milliseconds timeout_;
    HandlerFactory factory_;
    std::shared_ptr<SSL_CTX> context_;
    SocketOwner socket_;
    std::atomic_bool stopping_ = false;
    std::thread accept_thread_;
    std::vector<Session> sessions_;
};

TlsListener::TlsListener(std::string host, const std::uint16_t port,
                         std::filesystem::path certificate,
                         std::filesystem::path private_key,
                         const std::chrono::milliseconds timeout, HandlerFactory factory)
    : impl_(std::make_unique<Impl>(std::move(host), port, certificate, private_key,
                                   timeout, std::move(factory))) {}
TlsListener::~TlsListener() = default;
void TlsListener::start() { impl_->start(); }
void TlsListener::stop() noexcept { impl_->stop(); }

}
