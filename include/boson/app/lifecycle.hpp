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

/// @brief Graceful shutdown: closes every controller of envs and posts WM_QUIT with exit_code. Call on the UI
/// thread (hide the window first so the close looks instant).
///
/// With the default browser_exit_timeout of 0 the app does not wait for the WebView2 browser process: it is told
/// to close (it then flushes the profile and exits on its own, as it does for any host that goes away) and the
/// message loop ends at once. Measured: this removes up to 3 s of blocked shutdown (docs/PERFORMANCE.md). A
/// positive timeout waits up to that long for BrowserProcessExited first; do that when the process must not
/// outlive the profile it is using, e.g. before deleting the user data folder or in tests that reuse it.
/// relaunch() always waits (its successor needs the user data folder).
void quit(
    std::vector<std::shared_ptr<core::Environment>> envs = {},
    std::chrono::milliseconds browser_exit_timeout = std::chrono::milliseconds{0},
    int exit_code = 0);

/// @brief Registers work that exit_process() runs before it ends the process (flush the Logger, save a Store,
/// close a database). Hooks run in reverse registration order, each at most once, on the calling thread.
/// Anything not covered by a hook is lost by exit_process(): destructors do not run.
void add_exit_hook(std::function<void()> hook);

/// @brief Runs the registered exit hooks (once). Exceptions thrown by a hook are swallowed.
void run_exit_hooks() noexcept;

/// @brief Fast exit: run_exit_hooks(), flush the BOSON_TRACE table, then ExitProcess(exit_code). Skips the
/// destructors of the objects in wWinMain (scheduler thread joins, WebView and COM teardown) which cost
/// tens of milliseconds at the end of a session. Use as `return` replacement:
/// `app::exit_process(core::Window::run_message_loop());`
[[noreturn]] void exit_process(int exit_code);

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
