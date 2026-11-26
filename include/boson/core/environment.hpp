#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/ui_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {

class WebView;

/// @brief Options fixed for the lifetime of a WebView2 browser process.
///
/// Every field here is read once, when the browser process starts. Changing any of them for a running app
/// means calling Environment::recreate() (or app::relaunch()); assigning to the config afterwards has no effect.
struct EnvironmentConfig {
    std::wstring user_data_folder;
    std::wstring browser_executable_folder;           ///< Fixed-version runtime folder; empty = evergreen.
    std::vector<std::wstring> additional_browser_arguments; ///< Raw Chromium switches, e.g. L"--disable-gpu".
    std::vector<std::wstring> enable_features;        ///< Merged into one --enable-features= switch.
    std::vector<std::wstring> disable_features;       ///< Merged into one --disable-features= switch.
    std::wstring language;                            ///< put_Language, e.g. L"en-US".
    std::optional<bool> allow_single_sign_on;         ///< put_AllowSingleSignOnUsingOSPrimaryAccount
    std::optional<bool> exclusive_user_data_folder_access; ///< ICoreWebView2EnvironmentOptions2
    std::optional<bool> enable_tracking_prevention;   ///< ICoreWebView2EnvironmentOptions5
    std::optional<bool> browser_extensions_enabled;   ///< ICoreWebView2EnvironmentOptions6
    /// Adds the msWebView2EnableDraggableRegions feature so CSS app-region works on runtimes older than
    /// the ICoreWebView2Settings9 non-client-region API.
    bool enable_draggable_regions = false;
    bool register_boson_scheme = true;
    std::vector<std::wstring> boson_scheme_allowed_origins = {L"boson://app"};
};

/// @brief Builds the single AdditionalBrowserArguments string.
///
/// Chromium keeps only the last --enable-features / --disable-features switch, so every occurrence found in
/// additional_browser_arguments is merged with enable_features / disable_features into one switch each.
/// Duplicates are dropped (first occurrence wins the position) and a feature listed in both lists is only
/// disabled.
[[nodiscard]] std::wstring build_browser_arguments(const EnvironmentConfig& config);

enum class ProcessKind {
    Browser,
    Renderer,
    Utility,
    SandboxHelper,
    Gpu,
    PpapiPlugin,
    PpapiBroker,
    Unknown,
};

[[nodiscard]] std::string_view to_string(ProcessKind kind) noexcept;

struct ProcessInfo {
    std::uint32_t pid = 0;
    ProcessKind kind = ProcessKind::Unknown;
};

enum class BrowserExitKind {
    Normal,
    Failed,
};

struct BrowserProcessExit {
    BrowserExitKind kind = BrowserExitKind::Normal;
    std::uint32_t pid = 0;
};

/// @brief Owns one ICoreWebView2Environment (one browser process tree) shared by any number of WebViews.
///
/// All members must be called on the UI (STA) thread that created the environment.
class Environment : public std::enable_shared_from_this<Environment> {
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    using ReadyFn = std::function<void(ICoreWebView2Environment* env)>; ///< env is null when creation failed.
    using DoneFn = std::function<void(bool ok)>;

    /// @brief Starts environment creation immediately. COM must be initialized on the calling thread.
    [[nodiscard]] static std::shared_ptr<Environment> create(EnvironmentConfig config);

    Environment(PassKey, EnvironmentConfig config);
    ~Environment();

    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    Environment(Environment&&) = delete;
    Environment& operator=(Environment&&) = delete;

    /// @brief Runs fn once the environment exists (immediately when it already does).
    void when_ready(ReadyFn fn);

    [[nodiscard]] bool is_ready() const noexcept { return m_environment != nullptr && m_state == State::Ready; }
    [[nodiscard]] HRESULT creation_result() const noexcept { return m_creation_hr; }
    [[nodiscard]] ICoreWebView2Environment* get() const noexcept { return m_environment.Get(); }
    [[nodiscard]] const EnvironmentConfig& config() const noexcept { return m_config; }
    [[nodiscard]] const std::wstring& browser_arguments() const noexcept { return m_browser_arguments; }
    [[nodiscard]] std::size_t webview_count() const noexcept { return m_webviews.size(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

    /// @brief PID of the browser process, when it is running and can be determined.
    [[nodiscard]] std::optional<std::uint32_t> browser_process_id() const;
    /// @brief Snapshot of every process in this environment (ICoreWebView2Environment8::GetProcessInfos).
    [[nodiscard]] std::vector<ProcessInfo> process_infos() const;

    void set_process_infos_changed_callback(std::function<void()> cb);
    void set_browser_process_exited_callback(std::function<void(const BrowserProcessExit&)> cb);

    /// @brief Closes every registered WebView, waits for the browser process to exit (so the user data
    /// folder lock is released), creates a new environment from config and rebuilds the WebViews in place.
    void recreate(
        EnvironmentConfig config,
        DoneFn on_done = {},
        std::chrono::milliseconds exit_timeout = std::chrono::milliseconds{5000});

    /// @brief Closes every registered WebView and waits (up to timeout) for the browser process to exit.
    /// A timeout of 0 does not wait: the controllers are closed, the environment is released and on_done runs
    /// before shutdown() returns (app::quit() uses that).
    void shutdown(std::function<void()> on_done, std::chrono::milliseconds timeout = std::chrono::milliseconds{3000});

    // Called by WebView. Registration is what lets recreate()/shutdown() reach every controller.
    void register_webview(WebView* webview);
    void unregister_webview(WebView* webview) noexcept;

private:
    enum class State {
        Creating,
        Ready,
        Failed,
        TearingDown,
        Shutdown,
    };

    void start_creation();
    void on_created(HRESULT result, ICoreWebView2Environment* env);
    void attach_environment_events();
    void detach_environment_events() noexcept;
    void begin_teardown(std::chrono::milliseconds timeout, std::function<void()> after_exit);
    void finish_teardown();
    void flush_ready_queue();

    EnvironmentConfig m_config;
    std::wstring m_browser_arguments;
    State m_state = State::Creating;
    HRESULT m_creation_hr = E_PENDING;
    int m_retry_count = 0;
    std::uint64_t m_generation = 0;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    EventRegistrationToken m_exited_token{};
    EventRegistrationToken m_process_infos_token{};
    std::optional<std::uint32_t> m_last_browser_pid;

    std::vector<ReadyFn> m_ready_queue;
    std::vector<WebView*> m_webviews;
    std::vector<WebView*> m_rebuild_list;

    std::function<void()> m_on_process_infos_changed;
    std::function<void(const BrowserProcessExit&)> m_on_browser_exited;
    std::function<void()> m_after_exit;
    UiTimer m_timer;
};

} // namespace boson::core
