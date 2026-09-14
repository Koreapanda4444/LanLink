#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace lanlink::core {

enum class LogLevel {
    trace,
    debug,
    info,
    warning,
    error,
    off,
};

[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;
[[nodiscard]] LogLevel parse_log_level(std::string_view value);

class Logger {
public:
    Logger(std::filesystem::path file_path,
           std::string component,
           LogLevel minimum_level,
           std::uintmax_t max_size_bytes,
           std::size_t max_files);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void log(LogLevel level, std::string_view message);
    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warning(std::string_view message);
    void error(std::string_view message);
    void flush();

private:
    struct Record {
        std::chrono::system_clock::time_point timestamp;
        LogLevel level;
        std::string message;
    };

    void worker_loop() noexcept;
    void write_record(const Record& record);
    void rotate();
    void open_file();

    std::filesystem::path file_path_;
    std::string component_;
    LogLevel minimum_level_;
    std::uintmax_t max_size_bytes_;
    std::size_t max_files_;
    std::uintmax_t current_size_ = 0;
    std::ofstream stream_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable drained_;
    std::deque<Record> queue_;
    std::thread worker_;
    std::exception_ptr failure_;
    std::size_t dropped_records_ = 0;
    bool stopping_ = false;
    bool writing_ = false;
};

}
