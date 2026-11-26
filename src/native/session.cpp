#include <boson/native/session.hpp>

#include <boson/core/result.hpp>
#include <boson/native/notification.hpp>
#include <boson/native/tray.hpp>

namespace boson::native {

SessionEndInfo session_end_info(LPARAM l_param) noexcept {
    const auto flags = static_cast<std::uint64_t>(l_param);
    return SessionEndInfo{
        .logoff = (flags & ENDSESSION_LOGOFF) != 0,
        .close_app = (flags & ENDSESSION_CLOSEAPP) != 0,
        .critical = (flags & ENDSESSION_CRITICAL) != 0,
    };
}

// ---- SessionEndWatcher -----------------------------------------------------------------------------------

SessionEndWatcher::SessionEndWatcher(core::Window& window) : m_window(window) {
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) { return on_message(msg, w_param, l_param); });
}

SessionEndWatcher::~SessionEndWatcher() {
    clear_block_reason();
    m_window.remove_message_hook(m_hook);
}

bool SessionEndWatcher::set_block_reason(std::string_view reason) {
    if (reason.empty()) {
        clear_block_reason();
        return true;
    }
    m_reason = std::string(reason);
    if (m_window.hwnd() == nullptr) {
        return false;
    }
    const std::wstring wide = core::utf8_to_wide(reason);
    if (ShutdownBlockReasonCreate(m_window.hwnd(), wide.c_str()) == FALSE) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "ShutdownBlockReasonCreate");
        return false;
    }
    return true;
}

void SessionEndWatcher::clear_block_reason() noexcept {
    if (m_reason.empty()) {
        return;
    }
    m_reason.clear();
    if (m_window.hwnd() != nullptr) {
        ShutdownBlockReasonDestroy(m_window.hwnd());
    }
}

std::optional<LRESULT> SessionEndWatcher::on_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_QUERYENDSESSION) {
        Query query{.info = session_end_info(l_param), .allow = m_reason.empty()};
        if (m_on_query) {
            m_on_query(query);
        }
        if (query.info.critical) {
            return TRUE;
        }
        return query.allow ? TRUE : FALSE;
    }
    if (msg == WM_ENDSESSION) {
        const bool ending = w_param != FALSE;
        m_ending = ending;
        if (m_on_end) {
            m_on_end(ending, session_end_info(l_param));
        }
        return 0;
    }
    return std::nullopt;
}

// ---- CloseToTray -----------------------------------------------------------------------------------------

CloseToTray::CloseToTray(core::Window& window, TrayIcon& tray, Options options)
    : m_window(window), m_tray(tray), m_options(std::move(options)) {
    m_close_listener = m_window.on(core::WindowEventKind::CloseRequested, [this](core::WindowEvent& event) {
        if (!m_options.enabled || m_quitting) {
            return;
        }
        event.cancel = true;
        hide_to_tray();
    });
    if (!m_options.hide_tray_when_restored) {
        m_tray.show();
    }
}

CloseToTray::~CloseToTray() {
    m_window.off(m_close_listener);
}

void CloseToTray::hide_to_tray() {
    m_tray.show();
    m_window.hide();
    const bool changed = !m_hidden;
    m_hidden = true;
    if (!m_balloon_shown && (!m_options.first_hide_title.empty() || !m_options.first_hide_body.empty())) {
        m_balloon_shown = true;
        m_tray.show_balloon(NotificationOptions{
            .title = m_options.first_hide_title,
            .body = m_options.first_hide_body,
            .level = NotificationLevel::Info,
            .silent = true,
        });
    }
    if (changed && m_on_hidden) {
        m_on_hidden(true);
    }
}

void CloseToTray::restore() {
    m_window.show(IsIconic(m_window.hwnd()) != FALSE ? SW_RESTORE : SW_SHOW);
    m_window.focus();
    SetForegroundWindow(m_window.hwnd());
    const bool changed = m_hidden;
    m_hidden = false;
    if (m_options.hide_tray_when_restored) {
        m_tray.hide();
    }
    if (changed && m_on_hidden) {
        m_on_hidden(false);
    }
}

void CloseToTray::quit() {
    m_quitting = true;
    m_window.close();
}

bool CloseToTray::handle_tray_event(const TrayEvent& event) {
    if (!m_options.restore_on_click) {
        return false;
    }
    switch (event.kind) {
    case TrayEventKind::LeftClick:
    case TrayEventKind::DoubleClick:
    case TrayEventKind::KeyboardSelect:
    case TrayEventKind::BalloonClicked:
        restore();
        return true;
    default:
        return false;
    }
}

void CloseToTray::install_tray_callback(std::function<void(const TrayEvent&)> chained) {
    m_tray.set_event_callback([this, chained = std::move(chained)](const TrayEvent& event) {
        handle_tray_event(event);
        if (chained) {
            chained(event);
        }
    });
}

} // namespace boson::native
