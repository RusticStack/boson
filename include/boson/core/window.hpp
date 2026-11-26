#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {

enum class BackdropType {
    None,
    Mica,
    MicaAlt,
    Acrylic,
    Transparent,
    Translucent,
};

enum class CornerPreference {
    Default,
    Square,
    Round,
    RoundSmall,
};

struct WindowConfig {
    std::wstring title = L"Boson";
    int width = 1280;  ///< DIPs; scaled to the DPI of the monitor the window opens on.
    int height = 800;  ///< DIPs.
    int min_width = 0;  ///< DIPs of client area; 0 = no minimum. Enforced through WM_GETMINMAXINFO.
    int min_height = 0;
    bool borderless = false;
    bool headless_titlebar = false;
    bool dark_mode = true;
    bool always_on_top = false;
    bool center = false;
    /// Post WM_QUIT when this window is destroyed. Turn off for secondary windows.
    bool quit_on_destroy = true;
    HWND owner = nullptr;
    int titlebar_height = 0;
    /// Headless windows keep the invisible left/right/bottom resize borders non-client (like a native
    /// Windows 11 frame), so they stay usable above WebView HWNDs. Ignored for layered (Translucent) windows.
    bool native_resize_borders = true;
    BackdropType backdrop = BackdropType::None;
    CornerPreference corner_preference = CornerPreference::Round;
    float opacity = 1.0f;
};

enum class WindowEventKind {
    Resize,              ///< width/height = new client size (not sent while minimized)
    Move,                ///< x/y = client-area origin in screen coordinates
    Minimize,
    Maximize,
    Restore,
    Focus,
    Blur,
    DpiChanged,          ///< dpi = new DPI; the window has already been moved to the suggested rect
    CloseRequested,      ///< set cancel = true to veto WM_CLOSE
    Destroyed,
    EnterSizeMove,
    ExitSizeMove,
    MaximizeButtonHover, ///< flag = hovered (Snap Layouts maximize-button region)
    MaximizeButtonPress, ///< flag = pressed
};

struct WindowEvent {
    WindowEventKind kind = WindowEventKind::Resize;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool flag = false;
    bool cancel = false;
};

/// @brief A top-level Win32 window. All members must be called on the thread that created it.
class Window {
public:
    static constexpr UINT kWmBosonInvoke = WM_APP + 1;
    /// One-shot retry timer used when PostMessageW(kWmBosonInvoke) fails (message queue full).
    static constexpr UINT_PTR kInvokeRetryTimerId = 0xB05E;

    using ListenerId = std::uint64_t;
    using EventListener = std::function<void(WindowEvent& event)>;
    /// Returns a value to consume the message, std::nullopt to let Boson / DefWindowProc handle it.
    using MessageHook = std::function<std::optional<LRESULT>(UINT msg, WPARAM w_param, LPARAM l_param)>;

    explicit Window(WindowConfig config = {});
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    bool create(HINSTANCE instance = nullptr, int show_cmd = SW_SHOWDEFAULT);
    static int run_message_loop();

    void post_invoke() const noexcept;
    void close() const noexcept;
    void destroy() noexcept;
    void minimize() const noexcept;
    void maximize_or_restore() const noexcept;
    void start_drag() const noexcept;
    void show(int show_cmd = SW_SHOW) const noexcept;
    void hide() const noexcept;
    void focus() const noexcept;
    void center() const noexcept;

    bool set_backdrop(BackdropType backdrop) noexcept;
    bool set_opacity(float opacity) noexcept;
    bool set_dark_mode(bool enabled) noexcept;
    bool set_corner_preference(CornerPreference pref) noexcept;
    void set_always_on_top(bool enabled) noexcept;
    void set_fullscreen(bool enabled) noexcept;
    void set_title(std::wstring_view title) noexcept;
    /// @brief Minimum client size in DIPs (0 disables).
    void set_min_size(int min_width_dip, int min_height_dip) noexcept;

    /// @brief Outer window rect in screen pixels.
    [[nodiscard]] RECT bounds() const noexcept;
    void set_bounds(RECT screen_px) const noexcept;

    /// @brief Client-area rect (in client px) that behaves as the caption maximize button, so hovering it
    /// shows Windows 11 Snap Layouts. std::nullopt removes it. Usually driven by Boson.set_maximize_button_rect.
    void set_maximize_button_rect(std::optional<RECT> client_px);
    [[nodiscard]] std::optional<RECT> maximize_button_rect() const noexcept { return m_max_button_rect; }

    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }
    [[nodiscard]] RECT client_rect() const noexcept;
    [[nodiscard]] UINT dpi() const noexcept;
    [[nodiscard]] int dip_to_px(int dip) const noexcept;
    [[nodiscard]] int px_to_dip(int px) const noexcept;
    [[nodiscard]] bool is_maximized() const noexcept;
    [[nodiscard]] bool is_minimized() const noexcept;
    [[nodiscard]] bool is_visible() const noexcept;
    [[nodiscard]] bool is_fullscreen() const noexcept { return m_is_fullscreen; }
    [[nodiscard]] bool is_headless() const noexcept {
        return m_config.headless_titlebar || m_config.borderless;
    }
    [[nodiscard]] bool is_transparent_backdrop() const noexcept;
    [[nodiscard]] BackdropType backdrop() const noexcept { return m_config.backdrop; }
    [[nodiscard]] const WindowConfig& config() const noexcept { return m_config; }

    /// @brief Subscribes to one kind of window event. Safe to call from inside a listener.
    ListenerId on(WindowEventKind kind, EventListener listener);
    bool off(ListenerId id) noexcept;

    /// @brief Low-level hook run before Boson's own handling (used by WebViewHost for composition input).
    ListenerId add_message_hook(MessageHook hook);
    bool remove_message_hook(ListenerId id) noexcept;

    // Convenience wrappers over on().
    void set_resize_callback(std::function<void(int width, int height)> cb);
    void set_invoke_callback(std::function<void()> cb);
    /// @brief Called from WM_DESTROY.
    void set_close_callback(std::function<void()> cb);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(UINT msg, WPARAM w_param, LPARAM l_param);

    void emit(WindowEvent& event);
    void emit(WindowEventKind kind);
    void compact_listeners() noexcept;
    std::optional<LRESULT> run_message_hooks(UINT msg, WPARAM w_param, LPARAM l_param);

    void apply_dark_mode() const noexcept;
    void apply_corner_preference() const noexcept;
    void apply_backdrop() noexcept;
    void apply_borderless_frame() const noexcept;
    LRESULT handle_nccalcsize(WPARAM w_param, LPARAM l_param) const noexcept;
    [[nodiscard]] bool uses_native_resize_borders() const noexcept;
    LRESULT handle_nchittest(LPARAM l_param);
    LRESULT resize_border_hit(POINT screen_pt) const noexcept;
    bool point_in_max_button(POINT screen_pt) const noexcept;
    void set_max_button_hover(bool hovered);
    void handle_min_max_info(MINMAXINFO* info) const noexcept;

    WindowConfig m_config;
    HWND m_hwnd = nullptr;
    HWND m_caption_overlay = nullptr;
    bool m_is_fullscreen = false;
    WINDOWPLACEMENT m_saved_placement{};
    DWORD m_saved_style = 0;
    WPARAM m_size_state = SIZE_RESTORED;
    std::optional<RECT> m_max_button_rect;
    bool m_max_hovered = false;
    bool m_max_pressed = false;

    struct Listener {
        ListenerId id = 0;
        WindowEventKind kind = WindowEventKind::Resize;
        EventListener fn;
    };
    struct Hook {
        ListenerId id = 0;
        MessageHook fn;
    };
    std::vector<Listener> m_listeners;
    std::vector<Hook> m_hooks;
    ListenerId m_next_listener_id = 1;
    int m_dispatch_depth = 0;
    bool m_needs_compaction = false;

    std::function<void(int width, int height)> m_on_resize;
    std::function<void()> m_on_invoke;
    std::function<void()> m_on_close;
};

} // namespace boson::core
