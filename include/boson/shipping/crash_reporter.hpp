#pragma once

#include <windows.h>
#include <dbghelp.h>

#include <boson/shipping/logger.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {
class WebView;
struct ProcessFailedEvent;
} // namespace boson::core

namespace boson::shipping {

/// @brief A minidump left by a crash, plus its metadata sidecar ("<dump>.json").
struct CrashReport {
    std::filesystem::path dump;
    std::filesystem::path metadata;
    std::string app_name;
    std::string app_version;
    std::string reason;           ///< "exception", "terminate", "purecall", "invalid_parameter", "abort", "manual".
    std::uint32_t exception_code = 0;
    std::uint32_t pid = 0;
    std::uint32_t thread_id = 0;
    std::string timestamp;        ///< UTC ISO-8601.
    std::uint64_t dump_bytes = 0;
};

struct CrashReporterConfig {
    /// Folder for dumps. Default: %LOCALAPPDATA%\<app_name>\crashes.
    std::filesystem::path dump_dir;
    std::wstring app_name = L"BosonApp";
    std::string app_version;
    MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    /// Oldest dumps beyond this count are deleted when the reporter is installed.
    std::size_t max_dumps = 10;
    /// Also catch std::terminate, pure virtual calls, CRT invalid-parameter errors and abort().
    bool catch_crt_errors = true;
    /// Optional: called after the dump was written, still inside the crashing process. It must be
    /// async-signal-safe in spirit (no locks, no heap if you can avoid it). Usually leave empty and use
    /// process_pending_reports() on the next start instead.
    void (*on_crash_written)(const wchar_t* dump_path) noexcept = nullptr;
    std::shared_ptr<Logger> logger;
};

/// @brief Writes a minidump when the process crashes.
///
/// A dedicated writer thread is created by install() and sleeps on an event. The unhandled-exception
/// filter only records the exception pointers and wakes that thread, which calls MiniDumpWriteDump with a
/// healthy stack: this also works for stack overflows and avoids dumping from the faulting thread's
/// corrupted context. After the dump the process terminates with the exception code (no WER dialog).
/// One CrashReporter can be installed per process.
class CrashReporter {
public:
    explicit CrashReporter(CrashReporterConfig config = {});
    ~CrashReporter();

    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;
    CrashReporter(CrashReporter&&) = delete;
    CrashReporter& operator=(CrashReporter&&) = delete;

    /// @brief Installs the handlers; false when another reporter is installed or the writer thread failed.
    bool install();
    void uninstall() noexcept;
    [[nodiscard]] bool installed() const noexcept;

    /// @brief Writes a dump of the running process now (it keeps running). Returns the dump path.
    [[nodiscard]] std::filesystem::path write_dump_now(std::string_view reason = "manual");

    /// @brief Dumps already on disk (from earlier crashes), oldest first.
    [[nodiscard]] std::vector<CrashReport> pending_reports() const;
    /// @brief Hands each pending report to upload (your own code — Boson never sends anything anywhere).
    /// Reports for which it returns true are deleted. Blocking: call from a background thread.
    std::size_t process_pending_reports(const std::function<bool(const CrashReport&)>& upload);

    /// @brief Logs every ProcessFailed of webview (render / GPU / utility process crashes, with
    /// ICoreWebView2ProcessFailedEventArgs2 reason, exit code and description) and keeps calling any
    /// on_process_failed handler that was set before. Optional extra callback for your own handling.
    void attach_webview(core::WebView& webview, std::function<void(const core::ProcessFailedEvent&)> extra = {});

    [[nodiscard]] const std::filesystem::path& dump_dir() const noexcept { return m_dump_dir; }
    [[nodiscard]] const CrashReporterConfig& config() const noexcept { return m_config; }

    /// @brief One-line description of a WebView2 ProcessFailed event.
    [[nodiscard]] static std::string describe(const core::ProcessFailedEvent& event);

    /// Implementation detail (shared with the process-wide crash handlers).
    struct State;

private:
    CrashReporterConfig m_config;
    std::filesystem::path m_dump_dir;
    std::unique_ptr<State> m_state;
};

} // namespace boson::shipping
