#include "lanlink/transport/quic.hpp"

#include "lanlink/auth/handshake.hpp"
#include "lanlink/auth/identity.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/message.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/protocol/stream_decoder.hpp"
#include "lanlink/relay/network_control_channel.hpp"
#include "lanlink/transport/reconnect.hpp"

#include <msquic.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lanlink::transport {
namespace {

constexpr QUIC_UINT62 application_shutdown_code = 0;
constexpr QUIC_UINT62 authentication_shutdown_code = 1;

struct PendingSend {
    PendingSend(std::vector<std::byte> encoded,
                const QUIC_API_TABLE* api_table,
                const HQUIC connection_handle,
                const bool shutdown)
        : bytes(std::move(encoded)),
          api(api_table),
          connection(connection_handle),
          shutdown_after_send(shutdown) {
        buffer.Length = static_cast<std::uint32_t>(bytes.size());
        buffer.Buffer = reinterpret_cast<std::uint8_t*>(bytes.data());
    }

    std::vector<std::byte> bytes;
    QUIC_BUFFER buffer{};
    const QUIC_API_TABLE* api;
    HQUIC connection;
    bool shutdown_after_send;
};

std::string status_text(const QUIC_STATUS status) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(status);
    return output.str();
}

[[noreturn]] void throw_status(const std::string& operation, const QUIC_STATUS status) {
    throw std::runtime_error(operation + " failed with " + status_text(status));
}

QUIC_BUFFER alpn_buffer() noexcept {
    return {
        static_cast<std::uint32_t>(protocol::protocol_alpn.size()),
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(protocol::protocol_alpn.data())),
    };
}

QUIC_STATUS send_frame(const QUIC_API_TABLE* api,
                       const HQUIC stream,
                       const protocol::Frame& frame,
                       const HQUIC connection = nullptr,
                       const bool shutdown_after_send = false) noexcept {
    try {
        auto pending = std::make_unique<PendingSend>(
            protocol::encode_frame(frame), api, connection, shutdown_after_send);
        const auto status = api->StreamSend(stream,
                                            &pending->buffer,
                                            1,
                                            QUIC_SEND_FLAG_NONE,
                                            pending.get());

        if (QUIC_SUCCEEDED(status)) {
            static_cast<void>(pending.release());
        }

        return status;
    } catch (...) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
}

void complete_send(QUIC_STREAM_EVENT* event) noexcept {
    auto pending = std::unique_ptr<PendingSend>(
        static_cast<PendingSend*>(event->SEND_COMPLETE.ClientContext));

    if (pending && pending->shutdown_after_send && pending->connection != nullptr) {
        pending->api->ConnectionShutdown(pending->connection,
                                         QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                         authentication_shutdown_code);
    }
}

void clear_session_id(std::optional<auth::SessionId>& session_id) noexcept {
    if (session_id) {
        session_id->fill(std::byte{0});
        session_id.reset();
    }
}

QUIC_STATUS enable_authenticated_connection(const QUIC_API_TABLE* api,
                                            const HQUIC connection,
                                            const std::chrono::milliseconds idle_timeout,
                                            const std::uint32_t keep_alive_interval_ms) noexcept {
    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = static_cast<std::uint64_t>(idle_timeout.count());
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.KeepAliveIntervalMs = keep_alive_interval_ms;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    return api->SetParam(connection, QUIC_PARAM_CONN_SETTINGS, sizeof(settings), &settings);
}

void log_noexcept(core::Logger& logger,
                  const core::LogLevel level,
                  const std::string& message) noexcept {
    try {
        logger.log(level, message);
    } catch (...) {
    }
}

void validate_server_options(const QuicServerOptions& options) {
    if (options.listen_host.empty()) {
        throw std::invalid_argument("QUIC listen host is empty");
    }

    if (options.port == 0) {
        throw std::invalid_argument("QUIC listen port must be non-zero");
    }

    if (options.certificate_file.empty() || options.private_key_file.empty()) {
        throw std::invalid_argument("QUIC relay certificate and private key are required");
    }

    if (options.auth_token_file.empty()) {
        throw std::invalid_argument("QUIC relay authentication token is required");
    }

    if (options.handshake_timeout <= std::chrono::milliseconds::zero() ||
        options.authentication_timeout <= std::chrono::milliseconds::zero() ||
        options.idle_timeout <= std::chrono::milliseconds::zero() ||
        options.keep_alive_interval_ms == 0 ||
        options.keep_alive_interval_ms >=
            static_cast<std::uint64_t>(options.idle_timeout.count())) {
        throw std::invalid_argument("invalid QUIC server timeout settings");
    }
}

void validate_client_options(const QuicClientOptions& options) {
    if (options.relay_host.empty()) {
        throw std::invalid_argument("QUIC relay host is empty");
    }

    if (options.port == 0) {
        throw std::invalid_argument("QUIC relay port must be non-zero");
    }

    if (options.identity_file.empty() || options.auth_token_file.empty()) {
        throw std::invalid_argument("QUIC client identity and authentication token are required");
    }

    if (options.handshake_timeout <= std::chrono::milliseconds::zero() ||
        options.authentication_timeout <= std::chrono::milliseconds::zero() ||
        options.idle_timeout <= std::chrono::milliseconds::zero() ||
        options.keep_alive_interval_ms == 0 ||
        options.keep_alive_interval_ms >=
            static_cast<std::uint64_t>(options.idle_timeout.count())) {
        throw std::invalid_argument("invalid QUIC client timeout settings");
    }

    if (options.reconnect_initial_delay <= std::chrono::milliseconds::zero() ||
        options.reconnect_maximum_delay < options.reconnect_initial_delay) {
        throw std::invalid_argument("invalid QUIC reconnect settings");
    }
}

}

class QuicRelayServer::Impl {
public:
    Impl(QuicServerOptions options,
         core::Logger& logger,
         relay::NetworkControlChannel& control_channel)
        : options_(std::move(options)),
          logger_(logger),
          control_channel_(control_channel) {
        validate_server_options(options_);

        try {
            auth_token_ = std::make_unique<auth::AuthToken>(
                auth::AuthToken::load(options_.auth_token_file));
            initialize();
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl() {
        stop();
        release();
    }

    void start() {
        std::lock_guard lock(lifecycle_mutex_);

        if (running_.load()) {
            return;
        }

        if (shutdown_started_) {
            throw std::logic_error("QUIC relay cannot be restarted after stop");
        }

        QUIC_ADDR address{};

        if (!QuicAddrFromString(options_.listen_host.c_str(), options_.port, &address)) {
            throw std::invalid_argument("listen_host must be an IPv4 or IPv6 address");
        }

        auto status = api_->ListenerOpen(registration_, listener_callback, this, &listener_);

        if (QUIC_FAILED(status)) {
            listener_ = nullptr;
            throw_status("ListenerOpen", status);
        }

        auto alpn = alpn_buffer();
        status = api_->ListenerStart(listener_, &alpn, 1, &address);

        if (QUIC_FAILED(status)) {
            api_->ListenerClose(listener_);
            listener_ = nullptr;
            throw_status("ListenerStart", status);
        }

        running_.store(true);
        log_noexcept(logger_,
                     core::LogLevel::info,
                     "QUIC relay listening on " + options_.listen_host + ":" +
                         std::to_string(options_.port));
    }

    void stop() noexcept {
        std::lock_guard lock(lifecycle_mutex_);

        if (listener_ != nullptr) {
            api_->ListenerClose(listener_);
            listener_ = nullptr;
        }

        running_.store(false);

        if (!shutdown_started_ && registration_ != nullptr) {
            shutdown_started_ = true;
            api_->RegistrationShutdown(registration_,
                                       QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                       application_shutdown_code);
        }
    }

    [[nodiscard]] bool running() const noexcept {
        return running_.load();
    }

private:
    struct ConnectionContext {
        ConnectionContext(Impl* owner_value, const HQUIC connection)
            : owner(owner_value),
              handshake(*owner_value->auth_token_,
                        reinterpret_cast<std::uintptr_t>(connection),
                        auth::ServerHandshake::Clock::now() +
                            owner_value->options_.authentication_timeout) {
        }

        Impl* owner;
        std::mutex handshake_mutex;
        auth::ServerHandshake handshake;
        std::atomic_bool control_stream_started = false;
        std::atomic_bool closed = false;
    };

    struct StreamContext {
        StreamContext(Impl* owner_value,
                      const HQUIC connection_value,
                      ConnectionContext* connection_context_value)
            : owner(owner_value),
              connection(connection_value),
              connection_context(connection_context_value) {
        }

        Impl* owner;
        HQUIC connection;
        ConnectionContext* connection_context;
        protocol::FrameStreamDecoder decoder;
    };

    struct ActiveControlStream {
        auth::DeviceId device_id{};
        HQUIC connection = nullptr;
        HQUIC stream = nullptr;
    };

    bool register_control_stream(ConnectionContext* const context,
                                 const HQUIC connection,
                                 const HQUIC stream,
                                 const auth::DeviceId& device_id) {
        std::lock_guard lock(active_control_streams_mutex_);

        if (context->closed.load()) {
            return false;
        }

        active_control_streams_.push_back({device_id, connection, stream});
        return true;
    }

    void close_control_stream(const HQUIC stream, const bool app_close) noexcept {
        std::lock_guard lock(active_control_streams_mutex_);
        std::erase_if(active_control_streams_, [stream](const auto& active) {
            return active.stream == stream;
        });

        if (!app_close) {
            api_->StreamClose(stream);
        }
    }

    void route_control_events(const std::vector<relay::RoutedControlFrame>& events) noexcept {
        std::lock_guard lock(active_control_streams_mutex_);

        for (const auto& routed : events) {
            for (const auto& active : active_control_streams_) {
                if (active.device_id != routed.recipient_device_id) {
                    continue;
                }

                const auto status = send_frame(api_, active.stream, routed.frame);

                if (QUIC_FAILED(status)) {
                    log_noexcept(logger_,
                                 core::LogLevel::warning,
                                 "network event send failed: " + status_text(status));
                }
            }
        }
    }

    void initialize() {
        auto status = MsQuicOpen2(&api_);

        if (QUIC_FAILED(status)) {
            api_ = nullptr;
            throw_status("MsQuicOpen2", status);
        }

        const QUIC_REGISTRATION_CONFIG registration_config{
            "lanlink-relay",
            QUIC_EXECUTION_PROFILE_LOW_LATENCY,
        };
        status = api_->RegistrationOpen(&registration_config, &registration_);

        if (QUIC_FAILED(status)) {
            registration_ = nullptr;
            throw_status("RegistrationOpen", status);
        }

        QUIC_SETTINGS settings{};
        settings.HandshakeIdleTimeoutMs =
            static_cast<std::uint64_t>(options_.handshake_timeout.count());
        settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
        settings.IdleTimeoutMs =
            static_cast<std::uint64_t>(options_.authentication_timeout.count());
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.PeerBidiStreamCount = 1;
        settings.IsSet.PeerBidiStreamCount = TRUE;

        auto alpn = alpn_buffer();
        status = api_->ConfigurationOpen(registration_,
                                         &alpn,
                                         1,
                                         &settings,
                                         sizeof(settings),
                                         nullptr,
                                         &configuration_);

        if (QUIC_FAILED(status)) {
            configuration_ = nullptr;
            throw_status("ConfigurationOpen", status);
        }

        const auto certificate_path = options_.certificate_file.string();
        const auto private_key_path = options_.private_key_file.string();
        QUIC_CERTIFICATE_FILE certificate{
            private_key_path.c_str(),
            certificate_path.c_str(),
        };
        QUIC_CREDENTIAL_CONFIG credentials{};
        credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        credentials.CertificateFile = &certificate;
        status = api_->ConfigurationLoadCredential(configuration_, &credentials);

        if (QUIC_FAILED(status)) {
            throw_status("ConfigurationLoadCredential", status);
        }
    }

    void release() noexcept {
        if (configuration_ != nullptr && api_ != nullptr) {
            api_->ConfigurationClose(configuration_);
            configuration_ = nullptr;
        }

        if (registration_ != nullptr && api_ != nullptr) {
            api_->RegistrationClose(registration_);
            registration_ = nullptr;
        }

        if (api_ != nullptr) {
            MsQuicClose(api_);
            api_ = nullptr;
        }
    }

    static QUIC_STATUS QUIC_API listener_callback(HQUIC,
                                                   void* context,
                                                   QUIC_LISTENER_EVENT* event) {
        try {
            return static_cast<Impl*>(context)->on_listener_event(event);
        } catch (...) {
            return QUIC_STATUS_INTERNAL_ERROR;
        }
    }

    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection,
                                                     void* context,
                                                     QUIC_CONNECTION_EVENT* event) {
        try {
            auto* connection_context = static_cast<ConnectionContext*>(context);
            return connection_context->owner->on_connection_event(
                connection, connection_context, event);
        } catch (...) {
            return QUIC_STATUS_INTERNAL_ERROR;
        }
    }

    static QUIC_STATUS QUIC_API stream_callback(HQUIC stream,
                                                 void* context,
                                                 QUIC_STREAM_EVENT* event) {
        try {
            auto* stream_context = static_cast<StreamContext*>(context);
            return stream_context->owner->on_stream_event(stream, stream_context, event);
        } catch (...) {
            return QUIC_STATUS_INTERNAL_ERROR;
        }
    }

    QUIC_STATUS on_listener_event(QUIC_LISTENER_EVENT* event) {
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
            return QUIC_STATUS_SUCCESS;
        }

        auto context = std::unique_ptr<ConnectionContext>(new (std::nothrow) ConnectionContext(
            this, event->NEW_CONNECTION.Connection));

        if (!context) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }

        auto* const raw_context = context.release();
        api_->SetCallbackHandler(
            event->NEW_CONNECTION.Connection,
            reinterpret_cast<void*>(
                static_cast<QUIC_CONNECTION_CALLBACK_HANDLER>(connection_callback)),
            raw_context);
        const auto status = api_->ConnectionSetConfiguration(
            event->NEW_CONNECTION.Connection, configuration_);

        if (QUIC_FAILED(status)) {
            api_->ConnectionClose(event->NEW_CONNECTION.Connection);
            delete raw_context;
            return status;
        }

        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS on_connection_event(HQUIC connection,
                                    ConnectionContext* context,
                                    QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                log_noexcept(logger_, core::LogLevel::info, "QUIC client connected");
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                log_noexcept(logger_,
                             core::LogLevel::warning,
                             "QUIC client transport shutdown: " +
                                 status_text(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                log_noexcept(logger_, core::LogLevel::info, "QUIC client disconnected");
                break;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                if (context->control_stream_started.exchange(true)) {
                    log_noexcept(logger_,
                                 core::LogLevel::warning,
                                 "duplicate authentication stream rejected");
                    api_->ConnectionShutdown(connection,
                                             QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                             authentication_shutdown_code);
                    break;
                }

                auto stream_context = std::unique_ptr<StreamContext>(
                    new (std::nothrow) StreamContext(this, connection, context));

                if (!stream_context) {
                    api_->ConnectionShutdown(connection,
                                             QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                             authentication_shutdown_code);
                    return QUIC_STATUS_OUT_OF_MEMORY;
                }

                auto* const raw_stream_context = stream_context.release();
                api_->SetCallbackHandler(
                    event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(
                        static_cast<QUIC_STREAM_CALLBACK_HANDLER>(stream_callback)),
                    raw_stream_context);
                break;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                {
                    std::lock_guard lock(active_control_streams_mutex_);
                    context->closed.store(true);
                    std::erase_if(active_control_streams_, [connection](const auto& active) {
                        return active.connection == connection;
                    });
                }

                {
                    std::lock_guard lock(context->handshake_mutex);
                    context->handshake.close();
                }

                api_->ConnectionClose(connection);
                delete context;
                break;
            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS on_stream_event(HQUIC stream,
                                StreamContext* context,
                                QUIC_STREAM_EVENT* event) {
        switch (event->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t index = 0; index < event->RECEIVE.BufferCount; ++index) {
                    const auto& input = event->RECEIVE.Buffers[index];

                    try {
                        bool authentication_expired = false;

                        {
                            std::lock_guard lock(
                                context->connection_context->handshake_mutex);
                            authentication_expired =
                                context->connection_context->handshake.expire();
                        }

                        if (authentication_expired) {
                            log_noexcept(logger_,
                                         core::LogLevel::warning,
                                         "relay authentication deadline expired");
                            api_->ConnectionShutdown(context->connection,
                                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                                     authentication_shutdown_code);
                            return QUIC_STATUS_SUCCESS;
                        }

                        const auto frames = context->decoder.push(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(input.Buffer), input.Length));

                        for (const auto& frame : frames) {
                            auth::DeviceId actor{};
                            bool authenticated = false;

                            {
                                std::lock_guard lock(
                                    context->connection_context->handshake_mutex);
                                auto& handshake = context->connection_context->handshake;
                                authenticated = handshake.authenticated();

                                if (authenticated) {
                                    actor = handshake.device_id();
                                }
                            }

                            if (authenticated) {
                                handle_control_frame(stream, actor, frame);
                            } else {
                                handle_auth_frame(stream, context, frame);
                            }
                        }
                    } catch (const std::exception& error) {
                        log_noexcept(logger_,
                                     core::LogLevel::warning,
                                     "invalid authentication stream: " +
                                         std::string(error.what()));
                        api_->ConnectionShutdown(context->connection,
                                                 QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                                 authentication_shutdown_code);
                        return QUIC_STATUS_SUCCESS;
                    }
                }
                break;
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                complete_send(event);
                break;
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
                api_->ConnectionShutdown(context->connection,
                                         QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                         authentication_shutdown_code);
                break;
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                close_control_stream(stream, event->SHUTDOWN_COMPLETE.AppCloseInProgress);
                delete context;
                break;
            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    void handle_auth_frame(const HQUIC stream,
                           StreamContext* context,
                           const protocol::Frame& frame) {
        protocol::Frame response;
        bool shutdown_after_send = false;
        bool authenticated = false;
        auth::DeviceId authenticated_device_id{};
        std::string device_id;

        {
            std::lock_guard lock(context->connection_context->handshake_mutex);
            auto& handshake = context->connection_context->handshake;

            if (frame.type == protocol::MessageType::client_hello) {
                response = handshake.handle_hello(frame);
            } else if (frame.type == protocol::MessageType::client_auth) {
                response = handshake.handle_proof(frame);
                shutdown_after_send = handshake.rejected();
                authenticated = handshake.authenticated();

                if (authenticated) {
                    authenticated_device_id = handshake.device_id();
                    device_id = handshake.device_id_hex();
                }
            } else {
                throw std::runtime_error("unexpected frame before authentication");
            }
        }

        if (frame.type == protocol::MessageType::client_auth) {
            if (authenticated) {
                control_channel_.note_authenticated(authenticated_device_id);
                const auto settings_status = enable_authenticated_connection(
                    api_,
                    context->connection,
                    options_.idle_timeout,
                    options_.keep_alive_interval_ms);

                if (QUIC_FAILED(settings_status)) {
                    throw std::runtime_error("authenticated connection settings failed: " +
                                             status_text(settings_status));
                }

                log_noexcept(logger_,
                             core::LogLevel::info,
                             "device authenticated: " + device_id);
            } else {
                log_noexcept(logger_, core::LogLevel::warning, "device authentication rejected");
            }
        }

        const auto status = send_frame(api_,
                                       stream,
                                       response,
                                       context->connection,
                                       shutdown_after_send);

        if (QUIC_FAILED(status)) {
            throw std::runtime_error("authentication response send failed: " +
                                     status_text(status));
        }

        if (authenticated &&
            !register_control_stream(context->connection_context,
                                     context->connection,
                                     stream,
                                     authenticated_device_id)) {
            throw std::runtime_error("authentication connection already closed");
        }
    }

    void handle_control_frame(const HQUIC stream,
                              const auth::DeviceId& actor,
                              const protocol::Frame& frame) {
        auto dispatch = control_channel_.handle_authenticated(actor, frame);
        const auto status = send_frame(api_, stream, dispatch.response);

        if (QUIC_FAILED(status)) {
            throw std::runtime_error("control response send failed: " + status_text(status));
        }

        route_control_events(dispatch.events);
    }

    QuicServerOptions options_;
    core::Logger& logger_;
    relay::NetworkControlChannel& control_channel_;
    std::unique_ptr<auth::AuthToken> auth_token_;
    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    std::mutex lifecycle_mutex_;
    std::mutex active_control_streams_mutex_;
    std::vector<ActiveControlStream> active_control_streams_;
    std::atomic_bool running_ = false;
    bool shutdown_started_ = false;
};

class QuicRelayClient::Impl {
public:
    Impl(QuicClientOptions options, core::Logger& logger)
        : options_(std::move(options)),
          logger_(logger),
          reconnect_(options_.reconnect_initial_delay, options_.reconnect_maximum_delay) {
        validate_client_options(options_);

        try {
            identity_ = std::make_unique<auth::DeviceIdentity>(
                auth::DeviceIdentity::load_or_create(options_.identity_file));
            auth_token_ = std::make_unique<auth::AuthToken>(
                auth::AuthToken::load(options_.auth_token_file));
            initialize();
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl() {
        stop();

        {
            std::unique_lock lock(run_mutex_);
            run_complete_.wait(lock, [this] { return !running_.load(); });
        }

        release();
    }

    void run(const std::stop_token stop_token) {
        bool expected = false;

        if (!running_.compare_exchange_strong(expected, true)) {
            throw std::logic_error("QUIC relay client is already running");
        }

        stop_requested_.store(false);
        connected_.store(false);
        authenticated_.store(false);
        std::stop_callback stop_callback(stop_token, [this] { stop(); });

        try {
            run_loop();
        } catch (...) {
            finish_run();
            throw;
        }

        finish_run();
    }

    void stop() noexcept {
        stop_requested_.store(true);
        delay_wake_.notify_all();

        std::shared_ptr<AttemptState> state;

        {
            std::lock_guard lock(active_mutex_);
            state = active_attempt_.lock();
        }

        if (state) {
            std::lock_guard lock(state->mutex);

            if (state->connection != nullptr && !state->shutdown_requested) {
                state->shutdown_requested = true;
                api_->ConnectionShutdown(state->connection,
                                         QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                         application_shutdown_code);
            }
        }
    }

    [[nodiscard]] bool running() const noexcept {
        return running_.load();
    }

    [[nodiscard]] bool connected() const noexcept {
        return connected_.load();
    }

    [[nodiscard]] bool authenticated() const noexcept {
        return authenticated_.load();
    }

    [[nodiscard]] std::optional<auth::SessionId> session_id() const noexcept {
        std::lock_guard lock(session_mutex_);
        return session_id_;
    }

private:
    struct AttemptState {
        std::mutex mutex;
        std::condition_variable complete_wake;
        HQUIC connection = nullptr;
        bool complete = false;
        bool ever_authenticated = false;
        bool shutdown_requested = false;
        std::optional<auth::SessionId> session_id;
    };

    struct ConnectionContext {
        Impl* owner;
        std::shared_ptr<AttemptState> state;
    };

    struct StreamContext {
        StreamContext(Impl* owner_value,
                      const HQUIC connection_value,
                      std::shared_ptr<AttemptState> state_value,
                      const auth::DeviceIdentity& identity,
                      const auth::AuthToken& token,
                      const auth::ServerHandshake::Clock::time_point deadline_value)
            : owner(owner_value),
              connection(connection_value),
              state(std::move(state_value)),
              handshake(identity, token),
              deadline(deadline_value) {
        }

        Impl* owner;
        HQUIC connection;
        std::shared_ptr<AttemptState> state;
        auth::ClientHandshake handshake;
        auth::ServerHandshake::Clock::time_point deadline;
        protocol::FrameStreamDecoder decoder;
    };

    void initialize() {
        auto status = MsQuicOpen2(&api_);

        if (QUIC_FAILED(status)) {
            api_ = nullptr;
            throw_status("MsQuicOpen2", status);
        }

        const QUIC_REGISTRATION_CONFIG registration_config{
            "lanlink-service",
            QUIC_EXECUTION_PROFILE_LOW_LATENCY,
        };
        status = api_->RegistrationOpen(&registration_config, &registration_);

        if (QUIC_FAILED(status)) {
            registration_ = nullptr;
            throw_status("RegistrationOpen", status);
        }

        QUIC_SETTINGS settings{};
        settings.HandshakeIdleTimeoutMs =
            static_cast<std::uint64_t>(options_.handshake_timeout.count());
        settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
        settings.IdleTimeoutMs =
            static_cast<std::uint64_t>(options_.authentication_timeout.count());
        settings.IsSet.IdleTimeoutMs = TRUE;

        auto alpn = alpn_buffer();
        status = api_->ConfigurationOpen(registration_,
                                         &alpn,
                                         1,
                                         &settings,
                                         sizeof(settings),
                                         nullptr,
                                         &configuration_);

        if (QUIC_FAILED(status)) {
            configuration_ = nullptr;
            throw_status("ConfigurationOpen", status);
        }

        QUIC_CREDENTIAL_CONFIG credentials{};
        credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
        status = api_->ConfigurationLoadCredential(configuration_, &credentials);

        if (QUIC_FAILED(status)) {
            throw_status("ConfigurationLoadCredential", status);
        }
    }

    void release() noexcept {
        if (registration_ != nullptr && api_ != nullptr) {
            api_->RegistrationShutdown(registration_,
                                       QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                       application_shutdown_code);
        }

        if (configuration_ != nullptr && api_ != nullptr) {
            api_->ConfigurationClose(configuration_);
            configuration_ = nullptr;
        }

        if (registration_ != nullptr && api_ != nullptr) {
            api_->RegistrationClose(registration_);
            registration_ = nullptr;
        }

        if (api_ != nullptr) {
            MsQuicClose(api_);
            api_ = nullptr;
        }
    }

    void run_loop() {
        reconnect_.reset();

        while (!stop_requested_.load()) {
            const auto state = std::make_shared<AttemptState>();

            if (start_attempt(state)) {
                std::unique_lock lock(state->mutex);
                state->complete_wake.wait(lock, [&] {
                    return state->complete || stop_requested_.load();
                });

                if (stop_requested_.load() && state->connection != nullptr &&
                    !state->shutdown_requested) {
                    state->shutdown_requested = true;
                    api_->ConnectionShutdown(state->connection,
                                             QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                             application_shutdown_code);
                }

                state->complete_wake.wait(lock, [&] { return state->complete; });
            }

            clear_active(state);

            if (state->ever_authenticated) {
                reconnect_.reset();
            }

            if (stop_requested_.load()) {
                break;
            }

            const auto delay = reconnect_.next_delay();
            log_noexcept(logger_,
                         core::LogLevel::warning,
                         "reconnecting to relay in " + std::to_string(delay.count()) + " ms");
            std::unique_lock lock(delay_mutex_);
            delay_wake_.wait_for(lock, delay, [this] { return stop_requested_.load(); });
        }
    }

    bool start_attempt(const std::shared_ptr<AttemptState>& state) {
        auto context = std::unique_ptr<ConnectionContext>(
            new (std::nothrow) ConnectionContext{this, state});

        if (!context) {
            log_noexcept(logger_, core::LogLevel::error, "cannot allocate QUIC connection state");
            return false;
        }

        HQUIC connection = nullptr;
        auto status = api_->ConnectionOpen(
            registration_, connection_callback, context.get(), &connection);

        if (QUIC_FAILED(status)) {
            log_noexcept(logger_,
                         core::LogLevel::warning,
                         "ConnectionOpen failed: " + status_text(status));
            return false;
        }

        {
            std::lock_guard lock(state->mutex);
            state->connection = connection;
        }

        {
            std::lock_guard lock(active_mutex_);
            active_attempt_ = state;
        }

        ConnectionContext* raw_context = nullptr;

        {
            std::lock_guard lock(state->mutex);

            if (stop_requested_.load()) {
                state->connection = nullptr;
                state->complete = true;
            } else {
                raw_context = context.release();
                status = api_->ConnectionStart(connection,
                                               configuration_,
                                               QUIC_ADDRESS_FAMILY_UNSPEC,
                                               options_.relay_host.c_str(),
                                               options_.port);

                if (QUIC_FAILED(status)) {
                    state->connection = nullptr;
                    state->complete = true;
                }
            }
        }

        if (raw_context == nullptr) {
            api_->ConnectionClose(connection);
            clear_active(state);
            return false;
        }

        if (QUIC_FAILED(status)) {
            api_->ConnectionClose(connection);
            delete raw_context;
            clear_active(state);
            log_noexcept(logger_,
                         core::LogLevel::warning,
                         "ConnectionStart failed: " + status_text(status));
            return false;
        }

        log_noexcept(logger_,
                     core::LogLevel::info,
                     "connecting to QUIC relay " + options_.relay_host + ":" +
                         std::to_string(options_.port));
        return true;
    }

    void clear_active(const std::shared_ptr<AttemptState>& state) noexcept {
        std::lock_guard lock(active_mutex_);
        const auto active = active_attempt_.lock();

        if (active && active.get() == state.get()) {
            active_attempt_.reset();
        }
    }

    void finish_run() noexcept {
        connected_.store(false);
        authenticated_.store(false);
        clear_session();
        running_.store(false);
        run_complete_.notify_all();
    }

    void set_session(const auth::SessionId& session_id) noexcept {
        std::lock_guard lock(session_mutex_);
        session_id_ = session_id;
    }

    void clear_session() noexcept {
        std::lock_guard lock(session_mutex_);
        clear_session_id(session_id_);
    }

    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection,
                                                     void* context,
                                                     QUIC_CONNECTION_EVENT* event) {
        try {
            auto* connection_context = static_cast<ConnectionContext*>(context);
            return connection_context->owner->on_connection_event(
                connection, connection_context, event);
        } catch (...) {
            return QUIC_STATUS_INTERNAL_ERROR;
        }
    }

    static QUIC_STATUS QUIC_API stream_callback(HQUIC stream,
                                                 void* context,
                                                 QUIC_STREAM_EVENT* event) {
        try {
            auto* stream_context = static_cast<StreamContext*>(context);
            return stream_context->owner->on_stream_event(stream, stream_context, event);
        } catch (...) {
            return QUIC_STATUS_INTERNAL_ERROR;
        }
    }

    QUIC_STATUS on_connection_event(HQUIC connection,
                                    ConnectionContext* context,
                                    QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                connected_.store(true);
                authenticated_.store(false);
                clear_session();
                log_noexcept(logger_, core::LogLevel::info, "connected to QUIC relay");
                open_control_stream(connection, context->state);
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                connected_.store(false);
                authenticated_.store(false);
                clear_session();
                log_noexcept(logger_,
                             core::LogLevel::warning,
                             "relay transport shutdown: " +
                                 status_text(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                connected_.store(false);
                authenticated_.store(false);
                clear_session();
                log_noexcept(logger_, core::LogLevel::warning, "relay closed the connection");
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                connected_.store(false);
                authenticated_.store(false);
                clear_session();

                {
                    std::lock_guard lock(context->state->mutex);
                    clear_session_id(context->state->session_id);
                    context->state->connection = nullptr;
                }

                api_->ConnectionClose(connection);

                {
                    std::lock_guard lock(context->state->mutex);
                    context->state->complete = true;
                }

                clear_active(context->state);
                context->state->complete_wake.notify_all();
                delete context;
                break;
            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    void open_control_stream(const HQUIC connection,
                             const std::shared_ptr<AttemptState>& state) {
        auto context = std::unique_ptr<StreamContext>(
            new (std::nothrow) StreamContext(this,
                                             connection,
                                             state,
                                             *identity_,
                                             *auth_token_,
                                             auth::ServerHandshake::Clock::now() +
                                                 options_.authentication_timeout));

        if (!context) {
            api_->ConnectionShutdown(connection,
                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                     authentication_shutdown_code);
            return;
        }

        HQUIC stream = nullptr;
        auto status = api_->StreamOpen(connection,
                                       QUIC_STREAM_OPEN_FLAG_NONE,
                                       stream_callback,
                                       context.get(),
                                       &stream);

        if (QUIC_FAILED(status)) {
            log_noexcept(logger_,
                         core::LogLevel::warning,
                         "authentication StreamOpen failed: " + status_text(status));
            api_->ConnectionShutdown(connection,
                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                     authentication_shutdown_code);
            return;
        }

        auto* const raw_context = context.release();
        status = api_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);

        if (QUIC_FAILED(status)) {
            log_noexcept(logger_,
                         core::LogLevel::warning,
                         "authentication StreamStart failed: " + status_text(status));
            api_->StreamClose(stream);
            delete raw_context;
            api_->ConnectionShutdown(connection,
                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                     authentication_shutdown_code);
            return;
        }

        const auto hello = raw_context->handshake.begin(1);
        status = send_frame(api_, stream, hello);

        if (QUIC_FAILED(status)) {
            log_noexcept(logger_,
                         core::LogLevel::warning,
                         "authentication hello send failed: " + status_text(status));
            api_->StreamShutdown(stream,
                                 QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
                                 authentication_shutdown_code);
            api_->ConnectionShutdown(connection,
                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                     authentication_shutdown_code);
        }
    }

    QUIC_STATUS on_stream_event(HQUIC stream,
                                StreamContext* context,
                                QUIC_STREAM_EVENT* event) {
        switch (event->Type) {
            case QUIC_STREAM_EVENT_START_COMPLETE:
                if (QUIC_FAILED(event->START_COMPLETE.Status)) {
                    api_->ConnectionShutdown(context->connection,
                                             QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                             authentication_shutdown_code);
                }
                break;
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t index = 0; index < event->RECEIVE.BufferCount; ++index) {
                    const auto& input = event->RECEIVE.Buffers[index];

                    try {
                        if (!context->handshake.authenticated() &&
                            auth::ServerHandshake::Clock::now() >= context->deadline) {
                            log_noexcept(logger_,
                                         core::LogLevel::warning,
                                         "relay authentication deadline expired");
                            api_->ConnectionShutdown(context->connection,
                                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                                     authentication_shutdown_code);
                            return QUIC_STATUS_SUCCESS;
                        }

                        const auto frames = context->decoder.push(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(input.Buffer), input.Length));

                        for (const auto& frame : frames) {
                            if (context->handshake.authenticated()) {
                                handle_control_notification(frame);
                            } else {
                                handle_auth_frame(stream, context, frame);
                            }
                        }
                    } catch (const std::exception& error) {
                        log_noexcept(logger_,
                                     core::LogLevel::warning,
                                     "invalid relay authentication response: " +
                                         std::string(error.what()));
                        api_->ConnectionShutdown(context->connection,
                                                 QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                                 authentication_shutdown_code);
                        return QUIC_STATUS_SUCCESS;
                    }
                }
                break;
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                complete_send(event);
                break;
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
                api_->ConnectionShutdown(context->connection,
                                         QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                         authentication_shutdown_code);
                break;
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
                    api_->StreamClose(stream);
                }
                delete context;
                break;
            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    void handle_auth_frame(const HQUIC stream,
                           StreamContext* context,
                           const protocol::Frame& frame) {
        if (frame.type == protocol::MessageType::server_hello) {
            const auto response = context->handshake.handle_challenge(frame);
            const auto status = send_frame(api_, stream, response);

            if (QUIC_FAILED(status)) {
                throw std::runtime_error("authentication proof send failed: " +
                                         status_text(status));
            }

            return;
        }

        if (frame.type != protocol::MessageType::auth_result) {
            throw std::runtime_error("unexpected relay authentication frame");
        }

        if (!context->handshake.handle_result(frame)) {
            log_noexcept(logger_, core::LogLevel::warning, "relay authentication rejected");
            api_->ConnectionShutdown(context->connection,
                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                     authentication_shutdown_code);
            return;
        }

        const auto session_id = context->handshake.session_id();

        if (!session_id) {
            throw std::runtime_error("relay accepted authentication without a session id");
        }

        const auto settings_status = enable_authenticated_connection(api_,
                                                                      context->connection,
                                                                      options_.idle_timeout,
                                                                      options_.keep_alive_interval_ms);

        if (QUIC_FAILED(settings_status)) {
            throw std::runtime_error("authenticated connection settings failed: " +
                                     status_text(settings_status));
        }

        set_session(*session_id);

        {
            std::lock_guard lock(context->state->mutex);
            context->state->ever_authenticated = true;
            context->state->session_id = *session_id;
        }

        authenticated_.store(true);
        log_noexcept(logger_,
                     core::LogLevel::info,
                     "relay authentication completed for device " +
                         identity_->device_id_hex());
    }

    void handle_control_notification(const protocol::Frame& frame) {
        if (frame.type != protocol::MessageType::network_event || frame.request_id != 0) {
            throw std::runtime_error("unexpected relay control frame");
        }

        const auto event = protocol::decode_network_event(frame.payload);
        log_noexcept(logger_,
                     core::LogLevel::info,
                     "network event received: " +
                         std::string(protocol::network_event_kind_name(event.kind)));
    }

    QuicClientOptions options_;
    core::Logger& logger_;
    ReconnectBackoff reconnect_;
    std::unique_ptr<auth::DeviceIdentity> identity_;
    std::unique_ptr<auth::AuthToken> auth_token_;
    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    std::atomic_bool running_ = false;
    std::atomic_bool connected_ = false;
    std::atomic_bool authenticated_ = false;
    std::atomic_bool stop_requested_ = false;
    mutable std::mutex session_mutex_;
    std::optional<auth::SessionId> session_id_;
    std::mutex active_mutex_;
    std::weak_ptr<AttemptState> active_attempt_;
    std::mutex delay_mutex_;
    std::condition_variable delay_wake_;
    std::mutex run_mutex_;
    std::condition_variable run_complete_;
};

QuicRelayServer::QuicRelayServer(QuicServerOptions options,
                                core::Logger& logger,
                                relay::NetworkControlChannel& control_channel)
    : impl_(std::make_unique<Impl>(std::move(options), logger, control_channel)) {
}

QuicRelayServer::~QuicRelayServer() = default;

void QuicRelayServer::start() {
    impl_->start();
}

void QuicRelayServer::stop() noexcept {
    impl_->stop();
}

bool QuicRelayServer::running() const noexcept {
    return impl_->running();
}

QuicRelayClient::QuicRelayClient(QuicClientOptions options, core::Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), logger)) {
}

QuicRelayClient::~QuicRelayClient() = default;

void QuicRelayClient::run(const std::stop_token stop_token) {
    impl_->run(stop_token);
}

void QuicRelayClient::stop() noexcept {
    impl_->stop();
}

bool QuicRelayClient::running() const noexcept {
    return impl_->running();
}

bool QuicRelayClient::connected() const noexcept {
    return impl_->connected();
}

bool QuicRelayClient::authenticated() const noexcept {
    return impl_->authenticated();
}

std::optional<auth::SessionId> QuicRelayClient::session_id() const noexcept {
    return impl_->session_id();
}

}
