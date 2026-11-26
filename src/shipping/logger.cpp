#include <boson/shipping/logger.hpp>

#include <boson/core/result.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>

namespace boson::shipping {

namespace {

std::mutex g_default_mutex;
std::shared_ptr<Logger> g_default_logger;

} // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    case LogLevel::Off: return "OFF";
    }
    return "INFO";
}

LogLevel parse_log_level(std::string_view name, LogLevel fallback) noexcept {
    char lower[16]{};
    if (name.size() >= sizeof(lower)) {
        return fallback;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    const std::string_view value{lower, name.size()};
    if (value == "trace") return LogLevel::Trace;
    if (value == "debug") return LogLevel::Debug;
    if (value == "info") return LogLevel::Info;
    if (value == "warn" || value == "warning") return LogLevel::Warn;
    if (value == "error") return LogLevel::Error;
    if (value == "fatal") return LogLevel::Fatal;
    if (value == "off" || value == "none") return LogLevel::Off;
    return fallback;
}

Logger::Logger(LoggerConfig config)
    : m_config(std::move(config))
    , m_min_level(m_config.min_level) {
    m_config.max_files = std::max<std::uint32_t>(1, m_config.max_files);
    m_config.max_file_bytes = std::max<std::uint64_t>(1024, m_config.max_file_bytes);
    if (m_config.file_name.empty()) {
        m_config.file_name = L"boson.log";
    }
    m_writer = std::jthread([this](std::stop_token stop) { writer_loop(stop); });
}

Logger::~Logger() {
    m_writer.request_stop();
    m_wake.notify_all();
    if (m_writer.joinable()) {
        m_writer.join();
    }
    close_file();
}

std::string Logger::format_line(LogLevel level, std::string_view category, std::string_view message) {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char stamp[40]{};
    std::snprintf(
        stamp,
        sizeof(stamp),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds);

    std::string line;
    line.reserve(48 + category.size() + message.size());
    line.append(stamp);
    line.push_back(' ');
    const std::string_view lvl = to_string(level);
    line.append(lvl);
    line.append(6 - std::min<std::size_t>(lvl.size(), 5), ' ');
    line.push_back('[');
    line.append(category.empty() ? std::string_view{"app"} : category);
    line.append("] ");
    for (const char ch : message) {
        // One record per line: embedded line breaks would make the file ambiguous to parse.
        line.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
    }
    line.append(" (tid ");
    line.append(std::to_string(GetCurrentThreadId()));
    line.append(")\n");
    return line;
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    if (!enabled(level)) {
        return;
    }
    std::string line = format_line(level, category, message);
    if (m_config.mirror_to_debugger) {
        OutputDebugStringA(line.c_str());
    }
    {
        std::scoped_lock lock(m_mutex);
        if (m_queue.size() >= m_config.max_queued_lines) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_queue.push_back(std::move(line));
        ++m_enqueued_seq;
    }
    m_wake.notify_one();
}

void Logger::flush() {
    std::unique_lock lock(m_mutex);
    const std::uint64_t target = m_enqueued_seq;
    if (m_written_seq >= target) {
        return;
    }
    m_wake.notify_one();
    m_flushed.wait(lock, [this, target]() { return m_written_seq >= target; });
}

std::filesystem::path Logger::current_file() const {
    return m_config.directory / m_config.file_name;
}

std::filesystem::path Logger::rotated_path(std::uint32_t index) const {
    const std::filesystem::path name{m_config.file_name};
    std::wstring rotated = name.stem().wstring();
    rotated.push_back(L'.');
    rotated += std::to_wstring(index);
    rotated += name.extension().wstring();
    return m_config.directory / rotated;
}

void Logger::writer_loop(std::stop_token stop) {
    std::vector<std::string> batch;
    while (true) {
        std::uint64_t seq = 0;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, stop, [this]() { return !m_queue.empty(); });
            if (m_queue.empty()) {
                // Stop requested and nothing left to write.
                m_written_seq = m_enqueued_seq;
                m_flushed.notify_all();
                return;
            }
            batch.swap(m_queue);
            seq = m_enqueued_seq;
        }
        write_batch(batch);
        batch.clear();
        {
            std::scoped_lock lock(m_mutex);
            m_written_seq = seq;
        }
        m_flushed.notify_all();
    }
}

void Logger::write_batch(std::vector<std::string>& lines) {
    if (m_file == INVALID_HANDLE_VALUE && !open_file()) {
        return;
    }
    std::string chunk;
    const auto write_chunk = [this, &chunk]() {
        if (chunk.empty() || m_file == INVALID_HANDLE_VALUE) {
            chunk.clear();
            return;
        }
        DWORD written = 0;
        if (!WriteFile(m_file, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr)) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "Logger: WriteFile");
        }
        m_file_size += written;
        chunk.clear();
    };
    for (const auto& line : lines) {
        if (m_file_size + chunk.size() + line.size() > m_config.max_file_bytes && m_file_size + chunk.size() > 0) {
            write_chunk();
            rotate();
            if (m_file == INVALID_HANDLE_VALUE) {
                return;
            }
        }
        chunk += line;
    }
    write_chunk();
}

bool Logger::open_file() {
    std::error_code ec;
    std::filesystem::create_directories(m_config.directory, ec);
    const std::wstring path = current_file().wstring();
    m_file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (m_file == INVALID_HANDLE_VALUE) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "Logger: CreateFileW");
        return false;
    }
    LARGE_INTEGER size{};
    m_file_size = GetFileSizeEx(m_file, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
    return true;
}

void Logger::close_file() noexcept {
    if (m_file != INVALID_HANDLE_VALUE) {
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
    m_file_size = 0;
}

void Logger::rotate() {
    close_file();
    const std::wstring active = current_file().wstring();
    if (m_config.max_files <= 1) {
        DeleteFileW(active.c_str());
    } else {
        DeleteFileW(rotated_path(m_config.max_files - 1).c_str());
        for (std::uint32_t i = m_config.max_files - 1; i > 1; --i) {
            MoveFileExW(rotated_path(i - 1).c_str(), rotated_path(i).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (!MoveFileExW(active.c_str(), rotated_path(1).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "Logger: rotate MoveFileExW");
        }
    }
    open_file();
}

void Logger::set_default(std::shared_ptr<Logger> logger) {
    std::scoped_lock lock(g_default_mutex);
    g_default_logger = std::move(logger);
}

std::shared_ptr<Logger> Logger::get_default() {
    std::scoped_lock lock(g_default_mutex);
    return g_default_logger;
}

void log_message(const std::shared_ptr<Logger>& logger, LogLevel level, std::string_view category, std::string_view message) {
    if (logger) {
        logger->log(level, category, message);
        return;
    }
    if (const auto fallback = Logger::get_default()) {
        fallback->log(level, category, message);
        return;
    }
    if (level >= LogLevel::Info && level != LogLevel::Off) {
        OutputDebugStringA(Logger::format_line(level, category, message).c_str());
    }
}

} // namespace boson::shipping
