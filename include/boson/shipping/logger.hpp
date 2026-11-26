#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace boson::shipping {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;
/// @brief "trace" | "debug" | "info" | "warn"/"warning" | "error" | "fatal" | "off" (case-insensitive).
[[nodiscard]] LogLevel parse_log_level(std::string_view name, LogLevel fallback = LogLevel::Info) noexcept;

struct LoggerConfig {
    /// Folder for the log files; created on first write. Required.
    std::filesystem::path directory;
    /// Active file name. Rotated files are "<stem>.1<ext>" (newest) ... "<stem>.<max_files-1><ext>".
    std::wstring file_name = L"boson.log";
    std::uint64_t max_file_bytes = 5ull * 1024 * 1024;
    /// Total number of files kept, including the active one (minimum 1).
    std::uint32_t max_files = 5;
    LogLevel min_level = LogLevel::Info;
    /// Also send every line to OutputDebugStringA.
    bool mirror_to_debugger = true;
    /// Lines waiting for the writer thread; further lines are dropped (and counted) instead of blocking.
    std::size_t max_queued_lines = 16384;
};

/// @brief Asynchronous, size-rotating UTF-8 file logger.
///
/// log() formats one line on the caller's thread (no I/O, no lock held while formatting) and hands it to
/// a dedicated writer thread, so it is safe to call from the UI thread and from pool threads. A full queue
/// drops lines instead of blocking the caller.
class Logger {
public:
    explicit Logger(LoggerConfig config);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void log(LogLevel level, std::string_view category, std::string_view message);

    template <typename... Args>
    void write(LogLevel level, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        if (!enabled(level)) {
            return;
        }
        log(level, category, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void info(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Info, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Warn, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Error, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        write(LogLevel::Debug, category, fmt, std::forward<Args>(args)...);
    }

    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return level != LogLevel::Off && level >= m_min_level.load(std::memory_order_relaxed);
    }
    void set_min_level(LogLevel level) noexcept { m_min_level.store(level, std::memory_order_relaxed); }

    /// @brief Blocks until every line queued before this call has been written to the file (handed to the OS).
    void flush();

    [[nodiscard]] std::filesystem::path current_file() const;
    [[nodiscard]] std::uint64_t dropped_lines() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
    [[nodiscard]] const LoggerConfig& config() const noexcept { return m_config; }

    /// @brief Process-wide default used by Boson's shipping features when they are given no logger.
    static void set_default(std::shared_ptr<Logger> logger);
    [[nodiscard]] static std::shared_ptr<Logger> get_default();

    /// @brief Formats one log line exactly as it is written ("<UTC ISO-8601> <LEVEL> [category] message").
    [[nodiscard]] static std::string format_line(LogLevel level, std::string_view category, std::string_view message);

private:
    void writer_loop(std::stop_token stop);
    void write_batch(std::vector<std::string>& lines);
    bool open_file();
    void close_file() noexcept;
    void rotate();
    [[nodiscard]] std::filesystem::path rotated_path(std::uint32_t index) const;

    LoggerConfig m_config;
    std::atomic<LogLevel> m_min_level;
    std::atomic<std::uint64_t> m_dropped{0};

    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_flushed;
    std::vector<std::string> m_queue;
    std::uint64_t m_enqueued_seq = 0;
    std::uint64_t m_written_seq = 0;

    HANDLE m_file = INVALID_HANDLE_VALUE; // writer thread only
    std::uint64_t m_file_size = 0;        // writer thread only
    std::jthread m_writer;
};

/// @brief Logs through logger, else the default logger, else OutputDebugStringA.
void log_message(const std::shared_ptr<Logger>& logger, LogLevel level, std::string_view category, std::string_view message);

} // namespace boson::shipping
