#pragma once

#include <windows.h>

#include <boson/core/environment.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::app {

inline constexpr std::wstring_view kWaitPidSwitch = L"--boson-wait-pid=";

/// @brief Quotes one argument so CommandLineToArgvW round-trips it exactly.
[[nodiscard]] std::wstring quote_argument(std::wstring_view arg);
/// @brief Quoted executable path followed by quoted arguments.
[[nodiscard]] std::wstring build_command_line(std::wstring_view executable, std::span<const std::wstring> args);
/// @brief argv[1..] of the current process.
[[nodiscard]] std::vector<std::wstring> command_line_args();
/// @brief Full path of the running executable.
[[nodiscard]] std::wstring executable_path();
[[nodiscard]] std::optional<std::uint32_t> parse_wait_pid(std::span<const std::wstring> args) noexcept;

/// @brief Call at the very start of wWinMain, before single_instance() and before any WebView2 environment
/// is created. When this process was started by relaunch(), it blocks until the previous process (which
/// already waited for its own BrowserProcessExited) has exited, so its user data folder lock is released
/// and environment creation cannot fail with 0x8007139F. Returns false on timeout.
bool wait_for_previous_instance(std::chrono::milliseconds timeout = std::chrono::seconds{15});

/// @brief Graceful shutdown: closes every controller of envs, waits (up to timeout) for each browser
/// process to exit, then posts WM_QUIT with exit_code. Call on the UI thread.
void quit(
    std::vector<std::shared_ptr<core::Environment>> envs = {},
    std::chrono::milliseconds timeout = std::chrono::seconds{3},
    int exit_code = 0);

struct RelaunchOptions {
    std::vector<std::wstring> extra_args;
    bool keep_original_args = true;
    /// Original arguments to drop (exact match, or prefix match when the entry ends with '=').
    std::vector<std::wstring> remove_args;
    std::chrono::milliseconds shutdown_timeout{std::chrono::seconds{3}};
    int exit_code = 0;
};

/// @brief quit() and then start the same executable again with --boson-wait-pid=<this pid>.
bool relaunch(RelaunchOptions options, std::vector<std::shared_ptr<core::Environment>> envs = {});
bool relaunch(std::span<const std::wstring> extra_args, std::vector<std::shared_ptr<core::Environment>> envs = {});

/// @brief Named-mutex single instance with argv forwarding over WM_COPYDATA to a message-only window.
class SingleInstance {
public:
    using SecondInstanceFn = std::function<void(std::vector<std::wstring> args, std::wstring working_directory)>;

    explicit SingleInstance(std::wstring_view app_id);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    SingleInstance(SingleInstance&&) = delete;
    SingleInstance& operator=(SingleInstance&&) = delete;

    [[nodiscard]] bool is_primary() const noexcept { return m_primary; }
    /// @brief Primary only: receive forwarded launches on this (UI) thread.
    bool listen(SecondInstanceFn on_second_instance);
    /// @brief Secondary only: hands args to the primary and lets it take the foreground.
    bool forward_to_primary(
        std::span<const std::wstring> args,
        std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;

private:
    static LRESULT CALLBACK message_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

    std::wstring m_app_id;
    HANDLE m_mutex = nullptr;
    HWND m_window = nullptr;
    bool m_primary = false;
    SecondInstanceFn m_on_second_instance;
};

/// @brief Returns the primary instance (listening), or nullptr after forwarding this process's argv to an
/// already-running primary — the caller should then return from wWinMain.
[[nodiscard]] std::unique_ptr<SingleInstance> single_instance(
    std::wstring_view app_id,
    SingleInstance::SecondInstanceFn on_second_instance);

} // namespace boson::app
