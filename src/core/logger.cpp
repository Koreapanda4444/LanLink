#include "lanlink/core/logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace lanlink::core {
namespace {

constexpr std::size_t queue_capacity = 8192;

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return result;
}

std::string escape_json(std::string_view value) {
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string result;
    result.reserve(value.size());

    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);

        switch (character) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    result += "\\u00";
                    result.push_back(hex[(byte >> 4U) & 0x0fU]);
                    result.push_back(hex[byte & 0x0fU]);
                } else {
                    result.push_back(character);
                }
                break;
        }
    }

    return result;
}

std::string format_timestamp(const std::chrono::system_clock::time_point timestamp) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  timestamp.time_since_epoch()) %
                              1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};

#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
           << std::setw(3) << milliseconds.count() << 'Z';
    return output.str();
}

std::filesystem::path rotated_path(const std::filesystem::path& path, const std::size_t index) {
    auto result = path;
    result += "." + std::to_string(index);
    return result;
}

void remove_file(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);

    if (error) {
        throw std::filesystem::filesystem_error("cannot remove log file", path, error);
    }
}

void move_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    const bool source_exists = std::filesystem::exists(source, error);

    if (error) {
        throw std::filesystem::filesystem_error("cannot inspect log file", source, error);
    }

    if (!source_exists) {
        return;
    }

    remove_file(destination);
    std::filesystem::rename(source, destination, error);

    if (error) {
        throw std::filesystem::filesystem_error(
            "cannot rotate log file", source, destination, error);
    }
}

}

std::string_view log_level_name(const LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace:
            return "trace";
        case LogLevel::debug:
            return "debug";
        case LogLevel::info:
            return "info";
        case LogLevel::warning:
            return "warn";
        case LogLevel::error:
            return "error";
        case LogLevel::off:
            return "off";
    }

    return "unknown";
}

LogLevel parse_log_level(const std::string_view value) {
    const auto normalized = lowercase(value);

    if (normalized == "trace") {
        return LogLevel::trace;
    }
    if (normalized == "debug") {
        return LogLevel::debug;
    }
    if (normalized == "info") {
        return LogLevel::info;
    }
    if (normalized == "warn" || normalized == "warning") {
        return LogLevel::warning;
    }
    if (normalized == "error") {
        return LogLevel::error;
    }
    if (normalized == "off") {
        return LogLevel::off;
    }

    throw std::invalid_argument("invalid log level");
}

Logger::Logger(std::filesystem::path file_path,
               std::string component,
               const LogLevel minimum_level,
               const std::uintmax_t max_size_bytes,
               const std::size_t max_files)
    : file_path_(std::move(file_path)),
      component_(std::move(component)),
      minimum_level_(minimum_level),
      max_size_bytes_(max_size_bytes),
      max_files_(max_files) {
    if (file_path_.empty()) {
        throw std::invalid_argument("log file path is empty");
    }
    if (component_.empty()) {
        throw std::invalid_argument("log component is empty");
    }
    if (max_size_bytes_ == 0) {
        throw std::invalid_argument("log max size is zero");
    }
    if (max_files_ == 0) {
        throw std::invalid_argument("log max files is zero");
    }

    open_file();
    worker_ = std::thread(&Logger::worker_loop, this);
}

Logger::~Logger() {
    try {
        flush();
    } catch (...) {
    }

    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }

    wake_.notify_one();

    if (worker_.joinable()) {
        worker_.join();
    }
}

void Logger::log(const LogLevel level, const std::string_view message) {
    if (minimum_level_ == LogLevel::off || level == LogLevel::off ||
        static_cast<int>(level) < static_cast<int>(minimum_level_)) {
        return;
    }

    {
        std::lock_guard lock(mutex_);

        if (stopping_ || failure_) {
            return;
        }

        if (queue_.size() >= queue_capacity) {
            ++dropped_records_;
            return;
        }

        if (dropped_records_ != 0 && queue_.size() + 1 < queue_capacity) {
            queue_.push_back({std::chrono::system_clock::now(),
                              LogLevel::warning,
                              "dropped " + std::to_string(dropped_records_) + " log records"});
            dropped_records_ = 0;
        }

        queue_.push_back({std::chrono::system_clock::now(), level, std::string(message)});
    }

    wake_.notify_one();
}

void Logger::trace(const std::string_view message) {
    log(LogLevel::trace, message);
}

void Logger::debug(const std::string_view message) {
    log(LogLevel::debug, message);
}

void Logger::info(const std::string_view message) {
    log(LogLevel::info, message);
}

void Logger::warning(const std::string_view message) {
    log(LogLevel::warning, message);
}

void Logger::error(const std::string_view message) {
    log(LogLevel::error, message);
}

void Logger::flush() {
    std::unique_lock lock(mutex_);

    if (dropped_records_ != 0 && queue_.size() < queue_capacity) {
        queue_.push_back({std::chrono::system_clock::now(),
                          LogLevel::warning,
                          "dropped " + std::to_string(dropped_records_) + " log records"});
        dropped_records_ = 0;
        wake_.notify_one();
    }

    drained_.wait(lock, [this] {
        return failure_ || (queue_.empty() && !writing_);
    });

    if (failure_) {
        std::rethrow_exception(failure_);
    }

    stream_.flush();

    if (!stream_) {
        throw std::runtime_error("cannot flush log file: " + file_path_.string());
    }
}

void Logger::worker_loop() noexcept {
    while (true) {
        Record record;

        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });

            if (stopping_ && queue_.empty()) {
                break;
            }

            record = std::move(queue_.front());
            queue_.pop_front();
            writing_ = true;
        }

        try {
            write_record(record);
        } catch (...) {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
            queue_.clear();
            writing_ = false;
            drained_.notify_all();
            return;
        }

        {
            std::lock_guard lock(mutex_);
            writing_ = false;

            if (queue_.empty()) {
                drained_.notify_all();
            }
        }
    }

    stream_.flush();
}

void Logger::write_record(const Record& record) {
    std::string line;
    line.reserve(record.message.size() + component_.size() + 96);
    line += "{\"timestamp\":\"";
    line += format_timestamp(record.timestamp);
    line += "\",\"level\":\"";
    line += log_level_name(record.level);
    line += "\",\"component\":\"";
    line += escape_json(component_);
    line += "\",\"message\":\"";
    line += escape_json(record.message);
    line += "\"}\n";

    if (current_size_ != 0 && current_size_ + line.size() > max_size_bytes_) {
        rotate();
    }

    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));

    if (!stream_) {
        throw std::runtime_error("cannot write log file: " + file_path_.string());
    }

    current_size_ += line.size();

    if (record.level == LogLevel::warning || record.level == LogLevel::error) {
        stream_.flush();
    }
}

void Logger::rotate() {
    stream_.flush();
    stream_.close();

    if (max_files_ == 1) {
        remove_file(file_path_);
    } else {
        remove_file(rotated_path(file_path_, max_files_ - 1));

        for (std::size_t index = max_files_ - 1; index > 1; --index) {
            move_file(rotated_path(file_path_, index - 1), rotated_path(file_path_, index));
        }

        move_file(file_path_, rotated_path(file_path_, 1));
    }

    open_file();
}

void Logger::open_file() {
    const auto parent = file_path_.parent_path();

    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);

        if (error) {
            throw std::filesystem::filesystem_error("cannot create log directory", parent, error);
        }
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(file_path_, error);

    if (error) {
        throw std::filesystem::filesystem_error("cannot inspect log file", file_path_, error);
    }

    current_size_ = exists ? std::filesystem::file_size(file_path_, error) : 0;

    if (error) {
        throw std::filesystem::filesystem_error("cannot inspect log size", file_path_, error);
    }

    stream_.open(file_path_, std::ios::binary | std::ios::app);

    if (!stream_) {
        throw std::runtime_error("cannot open log file: " + file_path_.string());
    }
}

}
