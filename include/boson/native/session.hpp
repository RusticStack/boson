#pragma once

#include <windows.h>

#include <boson/core/window.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace boson::native {

class TrayIcon;
struct TrayEvent;

/// @brief Why the session is ending (ENDSESSION_* flags of WM_QUERYENDSESSION / WM_ENDSESSION).
struct SessionEndInfo {
    bool logoff = false;    ///< ENDSESSION_LOGOFF: the user logs off (otherwise shutdown / restart)
    bool close_app = false; ///< ENDSESSION_CLOSEAPP: an installer / Restart Manager wants the app closed
    bool critical = false;  ///< ENDSESSION_CRITICAL: the app is terminated whatever it answers
};

[[nodiscard]] SessionEndInfo session_end_info(LPARAM l_param) noexcept;

/// @brief WM_QUERYENDSESSION / WM_ENDSESSION for one top-level window (event-driven: a message hook).
///
/// Windows asks every top-level window before logoff / shutdown. While a block reason is set (unsaved work),
/// the query answers FALSE and ShutdownBlockReasonCreate shows the reason in the "apps are preventing
/// shutdown" screen; critical ends cannot be blocked. on_query may also veto. on_end runs from WM_ENDSESSION:
/// when ending is true the process is terminated shortly after the handler returns, so flush state
/// synchronously there (no async work, no UI). UI thread; destroy it before the Window.
class SessionEndWatcher {
public:
    struct Query {
        SessionEndInfo info;
        bool allow = true; ///< set false to ask Windows to wait (ignored for critical ends)
    };
    using QueryFn = std::function<void(Query& query)>;
    using EndFn = std::function<void(bool ending, const SessionEndInfo& info)>;

    explicit SessionEndWatcher(core::Window& window);
    ~SessionEndWatcher();

    SessionEndWatcher(const SessionEndWatcher&) = delete;
    SessionEndWatcher& operator=(const SessionEndWatcher&) = delete;
    SessionEndWatcher(SessionEndWatcher&&) = delete;
    SessionEndWatcher& operator=(SessionEndWatcher&&) = delete;

    void set_query_callback(QueryFn fn) { m_on_query = std::move(fn); }
    void set_end_callback(EndFn fn) { m_on_end = std::move(fn); }

    /// @brief Blocks non-critical session ends with this user-visible reason (ShutdownBlockReasonCreate).
    bool set_block_reason(std::string_view reason);
    void clear_block_reason() noexcept;
    [[nodiscard]] const std::string& block_reason() const noexcept { return m_reason; }
    /// @brief true between a WM_ENDSESSION(ending = TRUE) and process exit.
    [[nodiscard]] bool session_ending() const noexcept { return m_ending; }

private:
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);

    core::Window& m_window;
    core::Window::ListenerId m_hook = 0;
    std::string m_reason;
    bool m_ending = false;
    QueryFn m_on_query;
    EndFn m_on_end;
};

/// @brief "Close to tray": closing the window (caption X, Alt+F4, WM_CLOSE) hides it and keeps the app in
/// the notification area; clicking the tray icon brings it back. quit() closes for real.
///
/// The TrayIcon's event callback is not taken over: forward tray events with handle_tray_event() (e.g. from
/// MenuBackend::set_tray_listener), or call install_tray_callback() when nothing else uses it. UI thread;
/// destroy it before the Window and the TrayIcon.
class CloseToTray {
public:
    struct Options {
        bool enabled = true;
        bool restore_on_click = true;        ///< left click / double click / keyboard select restores
        bool hide_tray_when_restored = false;///< tray icon visible only while the window is hidden
        std::string first_hide_title;        ///< balloon shown the first time the window goes to the tray
        std::string first_hide_body;
    };

    CloseToTray(core::Window& window, TrayIcon& tray, Options options);
    CloseToTray(core::Window& window, TrayIcon& tray) : CloseToTray(window, tray, Options{}) {}
    ~CloseToTray();

    CloseToTray(const CloseToTray&) = delete;
    CloseToTray& operator=(const CloseToTray&) = delete;
    CloseToTray(CloseToTray&&) = delete;
    CloseToTray& operator=(CloseToTray&&) = delete;

    void set_enabled(bool enabled) noexcept { m_options.enabled = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return m_options.enabled; }
    [[nodiscard]] bool hidden() const noexcept { return m_hidden; }

    /// @brief Hides the window to the tray now.
    void hide_to_tray();
    /// @brief Shows, restores and focuses the window.
    void restore();
    /// @brief Disables close-to-tray and closes the window (WM_CLOSE reaches the app normally).
    void quit();
    /// @brief Restores on the configured clicks. Returns true when the event was used.
    bool handle_tray_event(const TrayEvent& event);
    /// @brief TrayIcon::set_event_callback(handle_tray_event) plus an optional chained callback.
    void install_tray_callback(std::function<void(const TrayEvent&)> chained = {});
    void set_hidden_callback(std::function<void(bool hidden)> fn) { m_on_hidden = std::move(fn); }

private:
    core::Window& m_window;
    TrayIcon& m_tray;
    Options m_options;
    core::Window::ListenerId m_close_listener = 0;
    bool m_hidden = false;
    bool m_balloon_shown = false;
    bool m_quitting = false;
    std::function<void(bool)> m_on_hidden;
};

} // namespace boson::native
