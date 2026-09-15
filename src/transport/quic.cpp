#include "lanlink/transport/quic.hpp"

#include "lanlink/core/logger.hpp"
#include "lanlink/protocol/message.hpp"
#include "lanlink/transport/reconnect.hpp"

#include <msquic.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace lanlink::transport {
namespace {

constexpr QUIC_UINT62 application_shutdown_code = 0;

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

    if (options.handshake_timeout <= std::chrono::milliseconds::zero() ||
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

    if (options.handshake_timeout <= std::chrono::milliseconds::zero() ||
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
    Impl(QuicServerOptions options, core::Logger& logger)
        : options_(std::move(options)), logger_(logger) {
        validate_server_options(options_);

        try {
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
        Impl* owner;
    };

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
        settings.IdleTimeoutMs = static_cast<std::uint64_t>(options_.idle_timeout.count());
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.KeepAliveIntervalMs = options_.keep_alive_interval_ms;
        settings.IsSet.KeepAliveIntervalMs = TRUE;

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

    QUIC_STATUS on_listener_event(QUIC_LISTENER_EVENT* event) {
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
            return QUIC_STATUS_SUCCESS;
        }

        auto context = std::unique_ptr<ConnectionContext>(new (std::nothrow) ConnectionContext{this});

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
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                api_->ConnectionClose(connection);
                delete context;
                break;
            default:
                break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    QuicServerOptions options_;
    core::Logger& logger_;
    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    std::mutex lifecycle_mutex_;
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

private:
    struct AttemptState {
        std::mutex mutex;
        std::condition_variable complete_wake;
        HQUIC connection = nullptr;
        bool complete = false;
        bool ever_connected = false;
        bool shutdown_requested = false;
    };

    struct ConnectionContext {
        Impl* owner;
        std::shared_ptr<AttemptState> state;
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
        settings.IdleTimeoutMs = static_cast<std::uint64_t>(options_.idle_timeout.count());
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.KeepAliveIntervalMs = options_.keep_alive_interval_ms;
        settings.IsSet.KeepAliveIntervalMs = TRUE;

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

            if (state->ever_connected) {
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
        running_.store(false);
        run_complete_.notify_all();
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

    QUIC_STATUS on_connection_event(HQUIC connection,
                                    ConnectionContext* context,
                                    QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                {
                    std::lock_guard lock(context->state->mutex);
                    context->state->ever_connected = true;
                }
                connected_.store(true);
                log_noexcept(logger_, core::LogLevel::info, "connected to QUIC relay");
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                connected_.store(false);
                log_noexcept(logger_,
                             core::LogLevel::warning,
                             "relay transport shutdown: " +
                                 status_text(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                connected_.store(false);
                log_noexcept(logger_, core::LogLevel::warning, "relay closed the connection");
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                connected_.store(false);

                {
                    std::lock_guard lock(context->state->mutex);
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

    QuicClientOptions options_;
    core::Logger& logger_;
    ReconnectBackoff reconnect_;
    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    std::atomic_bool running_ = false;
    std::atomic_bool connected_ = false;
    std::atomic_bool stop_requested_ = false;
    std::mutex active_mutex_;
    std::weak_ptr<AttemptState> active_attempt_;
    std::mutex delay_mutex_;
    std::condition_variable delay_wake_;
    std::mutex run_mutex_;
    std::condition_variable run_complete_;
};

QuicRelayServer::QuicRelayServer(QuicServerOptions options, core::Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), logger)) {
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

}
