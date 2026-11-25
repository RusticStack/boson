#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

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
    int width = 1280;
    int height = 800;
    bool borderless = false;
    bool headless_titlebar = false;
    bool dark_mode = true;
    bool always_on_top = false;
    int titlebar_height = 0;
    BackdropType backdrop = BackdropType::None;
    CornerPreference corner_preference = CornerPreference::Round;
    float opacity = 1.0f;
};

class Window {
public:
    static constexpr UINT kWmBosonInvoke = WM_APP + 1;

    explicit Window(WindowConfig config = {});
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    bool create(HINSTANCE instance = nullptr, int show_cmd = SW_SHOWDEFAULT);
    int run_message_loop();

    void post_invoke() const noexcept;
    void close() const noexcept;
    void minimize() const noexcept;
    void maximize_or_restore() const noexcept;
    void start_drag() const noexcept;

    bool set_backdrop(BackdropType backdrop) noexcept;
    bool set_opacity(float opacity) noexcept;
    bool set_dark_mode(bool enabled) noexcept;
    bool set_corner_preference(CornerPreference pref) noexcept;
    void set_always_on_top(bool enabled) noexcept;
    void set_fullscreen(bool enabled) noexcept;
    void set_title(std::wstring_view title) noexcept;

    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }
    [[nodiscard]] RECT client_rect() const noexcept;
    [[nodiscard]] bool is_maximized() const noexcept;
    [[nodiscard]] bool is_fullscreen() const noexcept { return m_is_fullscreen; }
    [[nodiscard]] bool is_headless() const noexcept {
        return m_config.headless_titlebar || m_config.borderless;
    }
    [[nodiscard]] bool is_transparent_backdrop() const noexcept;
    [[nodiscard]] BackdropType backdrop() const noexcept { return m_config.backdrop; }

    void set_resize_callback(std::function<void(int width, int height)> cb);
    void set_invoke_callback(std::function<void()> cb);
    void set_close_callback(std::function<void()> cb);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(UINT msg, WPARAM w_param, LPARAM l_param);

    void apply_dark_mode() const noexcept;
    void apply_corner_preference() const noexcept;
    void apply_backdrop() noexcept;
    void apply_borderless_frame() const noexcept;
    LRESULT handle_nccalcsize(WPARAM w_param, LPARAM l_param) const noexcept;
    LRESULT handle_nchittest(LPARAM l_param) const noexcept;

    WindowConfig m_config;
    HWND m_hwnd = nullptr;
    bool m_is_fullscreen = false;
    WINDOWPLACEMENT m_saved_placement{};
    DWORD m_saved_style = 0;
    std::function<void(int width, int height)> m_on_resize;
    std::function<void()> m_on_invoke;
    std::function<void()> m_on_close;
};

} // namespace boson::core
