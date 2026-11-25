#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace boson::core {

struct WindowConfig {
    std::wstring title = L"Boson";
    int width = 1280;
    int height = 800;
    bool borderless = false;
    bool dark_mode = true;
    int titlebar_height = 36;
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

    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }
    [[nodiscard]] RECT client_rect() const noexcept;

    void set_resize_callback(std::function<void(int width, int height)> cb);
    void set_invoke_callback(std::function<void()> cb);
    void set_close_callback(std::function<void()> cb);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    LRESULT handle_message(UINT msg, WPARAM w_param, LPARAM l_param);

    void apply_dark_mode() const noexcept;
    void apply_borderless_frame() const noexcept;
    LRESULT handle_nccalcsize(WPARAM w_param, LPARAM l_param) const noexcept;
    LRESULT handle_nchittest(LPARAM l_param) const noexcept;

    WindowConfig m_config;
    HWND m_hwnd = nullptr;
    std::function<void(int width, int height)> m_on_resize;
    std::function<void()> m_on_invoke;
    std::function<void()> m_on_close;
};

} // namespace boson::core
