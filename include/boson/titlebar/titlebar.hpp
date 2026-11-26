#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>
#include <boson/titlebar/caption_buttons.hpp>
#include <boson/titlebar/theme.hpp>
#include <boson/titlebar/window_chrome.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace boson::titlebar {

struct TitlebarOptions {
    /// Right-click on HTCAPTION areas (CSS app-region: drag mapped by the host, titlebar_height, the caption
    /// buttons) shows the window menu at the cursor. Headless windows only; native frames keep DefWindowProc.
    bool system_menu_on_right_click = true;
    /// Alt+Space shows the window menu under the titlebar, Alt+F4 closes, also while a WebView has focus.
    bool system_keys = true;
    /// Thin invisible strip over the top edge of a restored headless window so the top resize border keeps
    /// working above WebView HWNDs (corners resize diagonally).
    bool top_resize_grip = true;
};

/// @brief Overlay geometry in DIPs (= CSS px at zoom 1), the equivalent of env(titlebar-area-*).
struct TitlebarOverlayInfo {
    bool visible = false;
    double x = 0.0;      ///< titlebar area (free space beside the buttons)
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double buttons_x = 0.0; ///< occupied caption-button rect
    double buttons_y = 0.0;
    double buttons_width = 0.0;
    double buttons_height = 0.0;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool rtl = false;
    bool maximized = false;
    bool active = true;
};

/// @brief Everything a custom titlebar needs on one Window: native caption buttons (optional), theme
/// tracking, the window menu on right-click / Alt+Space, Alt+F4 inside WebViews, and the top resize grip.
/// UI thread; destroy before the Window (and before attached WebViews are destroyed is not required).
class Titlebar {
public:
    using OverlayListener = std::function<void(const TitlebarOverlayInfo& info)>;

    explicit Titlebar(core::Window& window, TitlebarOptions options = {});
    ~Titlebar();

    Titlebar(const Titlebar&) = delete;
    Titlebar& operator=(const Titlebar&) = delete;
    Titlebar(Titlebar&&) = delete;
    Titlebar& operator=(Titlebar&&) = delete;

    /// @brief Shows Boson-drawn caption buttons (std::nullopt hides them). Electron's setTitleBarOverlay.
    bool set_overlay(std::optional<TitlebarOverlayOptions> options);
    [[nodiscard]] TitlebarOverlayInfo overlay_info() const;
    void on_overlay_changed(OverlayListener listener) { m_overlay_listeners.push_back(std::move(listener)); }

    [[nodiscard]] const ThemeInfo& theme() const noexcept { return m_theme.current(); }
    [[nodiscard]] ThemeWatcher& theme_watcher() noexcept { return m_theme; }
    [[nodiscard]] CaptionButtons& caption_buttons() noexcept { return *m_buttons; }
    [[nodiscard]] core::Window& window() const noexcept { return m_window; }
    [[nodiscard]] HWND top_grip_hwnd() const noexcept { return m_grip; }

    /// @brief Forwards Alt+Space / Alt+F4 from a focused WebView (AcceleratorKeyPressed). Idempotent.
    bool attach_webview(core::WebView& webview);
    void detach_webview(core::WebView& webview) noexcept;

    /// @brief Window menu at a screen point / under the titlebar (Alt+Space position). Returns the command.
    UINT show_system_menu(POINT screen_pt);
    UINT show_system_menu_at_caption();
    /// @brief Runs an Alt+Space / Alt+F4 action now. Returns false for SystemKeyAction::None.
    bool handle_system_key(SystemKeyAction action);
    /// @brief Replaces TrackPopupMenuEx (tests, custom menus).
    void set_menu_tracker(MenuTracker tracker) { m_tracker = std::move(tracker); }

    /// @brief Overlay info for an arbitrary layout (pure; used by overlay_info()).
    [[nodiscard]] static TitlebarOverlayInfo make_overlay_info(const CaptionLayout& layout, int client_width, bool visible,
        bool maximized, bool active) noexcept;

private:
    static LRESULT CALLBACK grip_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);
    void update_grip();
    void notify_overlay();
    [[nodiscard]] int caption_height_px() const noexcept;

    core::Window& m_window;
    TitlebarOptions m_options;
    ThemeWatcher m_theme;
    std::unique_ptr<CaptionButtons> m_buttons;
    MenuTracker m_tracker;
    HWND m_grip = nullptr;
    UINT m_dpi = USER_DEFAULT_SCREEN_DPI;
    UINT m_system_key_msg = 0;
    core::Window::ListenerId m_hook = 0;
    std::vector<core::Window::ListenerId> m_listeners;
    std::vector<OverlayListener> m_overlay_listeners;

    struct Attached {
        std::uint64_t id = 0;
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        EventRegistrationToken token{};
    };
    std::vector<Attached> m_webviews;
};

} // namespace boson::titlebar
