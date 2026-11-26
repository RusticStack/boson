#pragma once

#include <windows.h>

#include <boson/core/environment.hpp>
#include <boson/core/protocol.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/webview_host.hpp>
#include <boson/core/window.hpp>
#include <boson/desktop/window_state.hpp>
#include <boson/exec/callback_sender.hpp>
#include <boson/ipc/bridge.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::desktop::detail {
class HiddenWindow;
} // namespace boson::desktop::detail

namespace boson::exec {
class TaskRunner;
} // namespace boson::exec

namespace boson::app {

class WindowManager;

/// @brief Per-window options of WindowManager::create. Sizes in window are DIPs (see core::WindowConfig);
/// core::WindowConfig::quit_on_destroy and owner are managed by the WindowManager and ignored here.
struct ManagedWindowConfig {
    /// Unique, 1..64 chars of [A-Za-z0-9_-:/.] (Tauri's label rules plus '.').
    std::string label;
    core::WindowConfig window;
    /// Outer position in physical screen pixels. Ignored when center is set or a saved state is restored.
    std::optional<int> x;
    std::optional<int> y;
    /// Centers on the owner window when there is one, else on the monitor the window opens on.
    bool center = false;
    bool visible = true;
    /// Activate when shown (false = SW_SHOWNOACTIVATE).
    bool focus = true;
    /// Keeps the window off the taskbar (it is owned by an invisible window; see docs/WINDOWS.md).
    bool skip_taskbar = false;
    /// Transparent DWM backdrop + transparent WebView background (per-pixel alpha through the page).
    bool transparent = false;
    /// Label of the owner window: an owned window stays above its owner, is minimized/hidden with it and
    /// is destroyed with it. Empty = top-level.
    std::string parent;
    /// Requires parent: the owner is disabled while this window lives and it opens centered on the owner.
    bool modal = false;
    /// Saves/restores placement per label with desktop::WindowStateKeeper.
    bool persist_state = false;
    /// Closing (X, Alt+F4, close()) hides the window instead (tray pattern); destroy() and quit() still close it.
    bool hide_on_close = false;
    /// While a close request is deferred (C++ CloseRequest::defer or the JS close guard), close anyway after
    /// this long. 0 = wait for the answer forever (a one-shot UI timer runs only while a request is pending).
    std::chrono::milliseconds close_confirm_timeout{0};
    /// Main WebView; std::nullopt = none (add WebViews later with ManagedWindow::add_webview).
    std::optional<core::WebViewConfig> webview;
    core::HostingMode hosting = core::HostingMode::Windowed;
    /// Attach this window's WebViews to the manager's Bridge (when one is set).
    bool attach_bridge = true;
};

enum class WindowManagerEventKind : std::uint8_t {
    Created,
    Closed,         ///< after the window is destroyed and removed; find(label) is already null
    Focus,
    Blur,
    Moved,
    Resized,
    Maximized,
    Minimized,
    Restored,
    Shown,
    Hidden,
    CloseRequested, ///< a close was deferred to the page (JS close guard); close_request_id identifies it
};

[[nodiscard]] std::string_view to_string(WindowManagerEventKind kind) noexcept;

struct WindowManagerEvent {
    WindowManagerEventKind kind = WindowManagerEventKind::Created;
    std::string_view label;     ///< valid during the callback only
    RECT bounds{};              ///< outer rect in physical screen pixels (Moved / Resized / Created)
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    std::uint64_t close_request_id = 0;
};

/// @brief Handed to WindowManager::on_close_requested handlers (UI thread, from WM_CLOSE).
class CloseRequest {
public:
    [[nodiscard]] std::string_view label() const noexcept { return m_label; }
    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    /// true while WindowManager::quit() closes everything (handlers cannot veto then).
    [[nodiscard]] bool is_quitting() const noexcept { return m_quitting; }
    /// Vetoes the close now.
    void prevent() noexcept { m_prevented = true; }
    /// Keeps the window open until WindowManager::resolve_close(label(), id(), allow) is called.
    void defer() noexcept { ++m_deferrals; }

private:
    friend class WindowManager;
    CloseRequest(std::string_view label, std::uint64_t id, bool quitting) noexcept
        : m_label(label), m_id(id), m_quitting(quitting) {}

    std::string_view m_label;
    std::uint64_t m_id = 0;
    bool m_quitting = false;
    bool m_prevented = false;
    int m_deferrals = 0;
};

/// @brief One window owned by a WindowManager: a core::Window, its WebViewHost (WebViews on the manager's
/// shared Environment) and its optional WindowStateKeeper. UI thread only.
class ManagedWindow {
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    ManagedWindow(PassKey, WindowManager& manager, ManagedWindowConfig config);
    ~ManagedWindow();

    ManagedWindow(const ManagedWindow&) = delete;
    ManagedWindow& operator=(const ManagedWindow&) = delete;
    ManagedWindow(ManagedWindow&&) = delete;
    ManagedWindow& operator=(ManagedWindow&&) = delete;

    [[nodiscard]] const std::string& label() const noexcept { return m_config.label; }
    [[nodiscard]] const ManagedWindowConfig& config() const noexcept { return m_config; }
    [[nodiscard]] core::Window& window() noexcept { return m_window; }
    [[nodiscard]] const core::Window& window() const noexcept { return m_window; }
    [[nodiscard]] HWND hwnd() const noexcept { return m_window.hwnd(); }
    /// @brief Created lazily with the first WebView (so WebView-less windows never start a browser process).
    [[nodiscard]] core::WebViewHost* host() noexcept { return m_host.get(); }
    /// @brief The first WebView, or null.
    [[nodiscard]] core::WebView* main_webview() const noexcept;
    [[nodiscard]] desktop::WindowStateKeeper* state_keeper() noexcept { return m_state.get(); }
    [[nodiscard]] bool is_modal() const noexcept { return m_config.modal && !m_config.parent.empty(); }
    [[nodiscard]] const std::string& parent_label() const noexcept { return m_config.parent; }
    [[nodiscard]] bool close_guard() const noexcept { return m_close_guard; }
    [[nodiscard]] bool is_closing() const noexcept { return m_pending_close.has_value(); }
    [[nodiscard]] bool is_destroyed() const noexcept { return m_destroyed; }

    /// @brief Adds a WebView on the manager's Environment (created on first use). Relative initial_url /
    /// dev_url are not rewritten here; see WindowManager::resolve_url. Attached to the Bridge when set.
    core::WebView& add_webview(core::WebViewConfig config, core::WebViewCallbacks callbacks = {});
    /// @brief Detaches from the Bridge and destroys the WebView on the next loop turn.
    void remove_webview(core::WebView& webview);

private:
    friend class WindowManager;

    struct PendingClose {
        std::uint64_t id = 0;
        int outstanding = 0;
    };

    WindowManager& m_manager;
    ManagedWindowConfig m_config;
    core::Window m_window;
    std::unique_ptr<core::WebViewHost> m_host;
    std::unique_ptr<desktop::WindowStateKeeper> m_state;
    std::vector<core::Window::ListenerId> m_listeners;
    std::optional<PendingClose> m_pending_close;
    std::vector<exec::Completion<bool>> m_close_waiters;
    core::UiTimer m_close_timer;
    HWND m_owner = nullptr;
    bool m_close_guard = false;
    bool m_force_close = false;
    bool m_destroyed = false;
    bool m_disabled_owner = false;
    bool m_state_applied = false;
};

struct WindowManagerOptions {
    /// Shared by every WebView of every window (one browser process tree). Null = created from
    /// environment_config when the first WebView is added.
    std::shared_ptr<core::Environment> environment;
    core::EnvironmentConfig environment_config;
    HINSTANCE instance = nullptr;
    /// Default boson://app lookup for every WebView (e.g. AssetRegistry::find).
    core::ProtocolHandler::AssetLookupFn asset_lookup;
    /// Dev server base ("http://localhost:5173"); relative window URLs resolve against it when set.
    std::optional<std::string> dev_url;
    /// Template for WebViews of windows created through WindowController (settings, scripts, CSP headers).
    core::WebViewConfig default_webview;
    /// Bridge every managed WebView is attached to (set_bridge() can set it later). Must be destroyed before
    /// the WindowManager (declare it after), or reset with set_bridge(nullptr) first.
    ipc::Bridge* bridge = nullptr;
    /// Attach options per WebView; window_label is filled in by the manager. Default: Bridge defaults.
    std::function<ipc::BridgeAttachOptions(const ManagedWindow& window, const core::WebView& webview)> attach_options;
    /// When the last window is gone: on_last_window_closed() if set, else app::quit({environment}).
    bool quit_on_last_window_closed = true;
    std::function<void()> on_last_window_closed;
    /// State files: state_directory / "window-state-<label>.json" when set, else
    /// %LOCALAPPDATA%\<app_name>\window-state-<label>.json.
    std::wstring app_name;
    std::filesystem::path state_directory;
    /// Debounced state writes run here when set.
    exec::TaskRunner* runner = nullptr;
    /// Injects window.__BOSON_WINDOW__ = {label} into every managed WebView (lets getCurrentWindow() be sync).
    bool inject_label_script = true;
};

using WindowResult = std::expected<ManagedWindow*, std::string>;

/// @brief Multi-window apps by string label (Electron BrowserWindow, Tauri WebviewWindow): creation with
/// per-window config, parent/owned and modal windows, state persistence per label, vetoable close with
/// asynchronous confirmation, hide-to-tray, quit on last window. Every window's WebViews share one
/// core::Environment, so extra windows add renderers, never a browser process.
///
/// All members are UI-thread only except wake_ui(), label_of_webview() and current_label().
class WindowManager {
public:
    using ListenerId = std::uint64_t;
    using EventListener = std::function<void(const WindowManagerEvent& event)>;
    using CloseRequestHandler = std::function<void(CloseRequest& request)>;

    explicit WindowManager(WindowManagerOptions options = {});
    ~WindowManager();

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;
    WindowManager(WindowManager&&) = delete;
    WindowManager& operator=(WindowManager&&) = delete;

    /// @brief Creates and (unless config.visible is false) shows a window. Fails on an invalid or duplicate
    /// label, an unknown parent, modal without parent, or CreateWindowExW failure.
    WindowResult create(ManagedWindowConfig config);

    [[nodiscard]] ManagedWindow* find(std::string_view label) const noexcept;
    [[nodiscard]] ManagedWindow* find_by_webview(std::uint64_t webview_id) const noexcept;
    [[nodiscard]] ManagedWindow* find_by_hwnd(HWND hwnd) const noexcept;
    /// @brief The managed window that is the foreground window, or null.
    [[nodiscard]] ManagedWindow* focused() const noexcept;
    [[nodiscard]] std::span<const std::unique_ptr<ManagedWindow>> windows() const noexcept { return m_windows; }
    [[nodiscard]] std::vector<std::string> labels() const;
    /// @brief Live windows (destroyed windows awaiting cleanup are not counted).
    [[nodiscard]] std::size_t size() const noexcept;

    bool show(std::string_view label, bool focus = true);
    bool hide(std::string_view label);
    bool focus(std::string_view label);
    /// @brief Requests a close: close handlers / the JS guard may veto or defer it; hide_on_close hides.
    bool close(std::string_view label);
    /// @brief Closes without asking anyone (Electron's destroy()).
    bool destroy(std::string_view label);
    /// @brief Sender form of close(): completes with true once the window is destroyed, false when the
    /// close was vetoed or turned into hide_on_close. Never sync_wait it on the UI thread.
    [[nodiscard]] exec::CallbackSender<bool> close_async(std::string label);

    /// @brief Answers a deferred close. allow = false vetoes it; true counts one deferral down and closes
    /// when none is left. Returns false when no such request is pending.
    bool resolve_close(std::string_view label, std::uint64_t request_id, bool allow);
    /// @brief When enabled, closing the window defers to the page: a CloseRequested event (window:close-
    /// requested) is emitted and the page answers with resolve_close (Window.confirm_close).
    bool set_close_guard(std::string_view label, bool enabled);
    /// @brief Adds / removes the window's taskbar button at runtime (ITaskbarList). For windows created with
    /// skip_taskbar the invisible owner already keeps the button away.
    bool set_skip_taskbar(std::string_view label, bool enabled);

    ListenerId on_close_requested(CloseRequestHandler handler);
    ListenerId on_event(EventListener listener);
    bool off(ListenerId id) noexcept;

    /// @brief Destroys every window (force) without running the last-window action.
    void destroy_all();
    /// @brief destroy_all() then app::quit({environment}, timeout, exit_code).
    void quit(int exit_code = 0, std::chrono::milliseconds timeout = std::chrono::seconds{3});
    [[nodiscard]] bool is_quitting() const noexcept { return m_quitting; }
    void set_quit_on_last_window_closed(bool enabled) noexcept { m_options.quit_on_last_window_closed = enabled; }
    void set_on_last_window_closed(std::function<void()> fn) { m_options.on_last_window_closed = std::move(fn); }

    /// @brief The shared Environment, created on first call from options.environment_config.
    [[nodiscard]] const std::shared_ptr<core::Environment>& environment();
    [[nodiscard]] bool has_environment() const noexcept { return m_options.environment != nullptr; }
    void set_bridge(ipc::Bridge* bridge);
    [[nodiscard]] ipc::Bridge* bridge() const noexcept { return m_options.bridge; }
    [[nodiscard]] const WindowManagerOptions& options() const noexcept { return m_options; }

    /// @brief Makes a URL for a new window: "" = boson://app/index.html; relative paths resolve against
    /// options.dev_url when set, else boson://app/; absolute URLs are returned unchanged.
    [[nodiscard]] std::string resolve_url(std::string_view url) const;
    /// @brief true for labels WindowManager accepts.
    [[nodiscard]] static bool is_valid_label(std::string_view label) noexcept;

    // ---- Any thread ---------------------------------------------------------------------------------------
    /// @brief Wakes the UI thread: posts to a message-only window that outlives every managed window. Use
    /// it as the UiMarshal wake function so marshaled work survives the main window closing.
    void wake_ui() const noexcept;
    /// @brief Runs on the UI thread after each wake_ui() (e.g. marshal.drain_on_ui_thread()).
    void set_invoke_callback(std::function<void()> fn);
    /// @brief Label of the window hosting webview_id.
    [[nodiscard]] std::optional<std::string> label_of_webview(std::uint64_t webview_id) const;
    /// @brief Label of the window whose WebView sent the IPC request being handled on this thread
    /// (ipc::current_sender_id(), carried to the pool thread by Router).
    [[nodiscard]] std::optional<std::string> current_label() const;

private:
    friend class ManagedWindow;

    struct Handler {
        ListenerId id = 0;
        CloseRequestHandler close;
        EventListener event;
    };

    void wire(ManagedWindow& window);
    void emit(WindowManagerEventKind kind, const ManagedWindow& window, std::uint64_t request_id = 0);
    void on_close_message(ManagedWindow& window, core::WindowEvent& event);
    void finish_close(ManagedWindow& window, bool allow_hide);
    void complete_waiters(ManagedWindow& window, bool closed);
    void cancel_close(ManagedWindow& window);
    void on_destroyed(ManagedWindow& window);
    void release_owner(ManagedWindow& window) noexcept;
    void reap();
    void register_webview(const ManagedWindow& window, core::WebView& webview);
    void unregister_webview(std::uint64_t webview_id) noexcept;
    [[nodiscard]] HWND taskbar_owner();
    void place_new_window(ManagedWindow& window, HWND owner);

    WindowManagerOptions m_options;
    std::vector<std::unique_ptr<ManagedWindow>> m_windows;
    std::vector<Handler> m_handlers;
    ListenerId m_next_id = 1;
    std::uint64_t m_next_close_id = 1;
    int m_dispatch_depth = 0;
    bool m_quitting = false;
    bool m_suppress_last_window = false;
    bool m_reap_posted = false;

    std::unique_ptr<desktop::detail::HiddenWindow> m_message_window; // wake + deferred cleanup
    std::unique_ptr<desktop::detail::HiddenWindow> m_taskbar_owner;  // lazily, for skip_taskbar
    HWND m_message_hwnd = nullptr;                                    // read from any thread
    std::function<void()> m_on_invoke;

    struct OwnerDisable {
        HWND owner = nullptr;
        int count = 0;
    };
    std::vector<OwnerDisable> m_disabled_owners;

    mutable std::shared_mutex m_registry_mutex;
    std::vector<std::pair<std::uint64_t, std::string>> m_webview_labels; // guarded by m_registry_mutex
};

} // namespace boson::app
