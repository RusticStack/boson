#pragma once

#include <windows.h>

#include <boson/core/composition.hpp>
#include <boson/core/environment.hpp>
#include <boson/core/ui_timer.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace boson::core {

struct LayoutContext {
    RECT client_px{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool minimized = false;

    [[nodiscard]] int px(int dip) const noexcept { return MulDiv(dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }
    [[nodiscard]] int dip(int px_value) const noexcept { return MulDiv(px_value, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi)); }
    [[nodiscard]] int width() const noexcept { return client_px.right - client_px.left; }
    [[nodiscard]] int height() const noexcept { return client_px.bottom - client_px.top; }
};

/// @brief Tab sleeping: a WebView hidden for hidden_timeout that is not playing audio gets a Low memory
/// target and is suspended (state is kept; set_visible(true) resumes it). Destroy a WebView instead when
/// you can afford to lose its in-page state and want the renderer's memory back completely.
struct SleepPolicy {
    std::chrono::milliseconds hidden_timeout{std::chrono::minutes{5}};
    std::chrono::milliseconds check_interval{std::chrono::seconds{30}};
    bool lower_memory_target = true;
    bool suspend = true;
};

struct WebViewCallbacks {
    ProtocolHandler::AssetLookupFn asset_lookup;
    WebView::MessageCallback on_message;
    WebView::ReadyCallback on_ready;
};

/// @brief Owns the WebViews of one Window: layout on WM_SIZE / WM_DPICHANGED, NotifyParentWindowPositionChanged
/// on every move, visibility on minimize/restore, z-order, composition input forwarding and CSS app-region
/// hit testing, and the optional SleepPolicy. UI thread only.
class WebViewHost {
public:
    using LayoutFn = std::function<void(WebViewHost& host, const LayoutContext& context)>;

    WebViewHost(Window& window, std::shared_ptr<Environment> env, HostingMode mode = HostingMode::Windowed);
    ~WebViewHost();

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;
    WebViewHost(WebViewHost&&) = delete;
    WebViewHost& operator=(WebViewHost&&) = delete;

    /// @brief Creates and initializes a WebView in this window (config.hosting/composition are filled in).
    WebView& create(WebViewConfig config, WebViewCallbacks callbacks = {});
    /// @brief Hides webview now and destroys it on the next message-loop turn, so it is safe to call from
    /// inside one of that WebView's own event callbacks.
    void destroy(WebView& webview);

    [[nodiscard]] std::span<const std::unique_ptr<WebView>> webviews() const noexcept { return m_webviews; }
    [[nodiscard]] WebView* find(std::uint64_t id) const noexcept;
    /// @brief Top-most visible WebView containing client_px (composition z-order, else creation order).
    [[nodiscard]] WebView* webview_at(POINT client_px) const noexcept;

    void set_layout(LayoutFn layout);
    void relayout();
    [[nodiscard]] LayoutContext layout_context() const noexcept;

    void set_default_asset_lookup(ProtocolHandler::AssetLookupFn lookup) { m_default_lookup = std::move(lookup); }
    void set_on_destroy(std::function<void(WebView&)> cb) { m_on_destroy = std::move(cb); }

    void set_sleep_policy(std::optional<SleepPolicy> policy);
    void set_sleep_exempt(WebView& webview, bool exempt);
    void set_on_slept(std::function<void(WebView&)> cb) { m_on_slept = std::move(cb); }
    [[nodiscard]] bool is_sleeping(const WebView& webview) const noexcept;
    void check_sleep_now();

    [[nodiscard]] Window& window() const noexcept { return m_window; }
    [[nodiscard]] const std::shared_ptr<Environment>& environment() const noexcept { return m_env; }
    [[nodiscard]] HostingMode hosting() const noexcept { return m_mode; }
    [[nodiscard]] const std::shared_ptr<CompositionHost>& composition() const noexcept { return m_composition; }

private:
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);
    std::optional<LRESULT> on_mouse(UINT msg, WPARAM w_param, LPARAM l_param);
    std::optional<LRESULT> on_nc_hit_test(LPARAM l_param) const;
    void flush_graveyard();
    void set_hover(WebView* webview);

    Window& m_window;
    std::shared_ptr<Environment> m_env;
    HostingMode m_mode;
    std::shared_ptr<CompositionHost> m_composition;
    std::vector<std::unique_ptr<WebView>> m_webviews;
    std::vector<std::unique_ptr<WebView>> m_graveyard;
    UiTimer m_graveyard_timer;
    LayoutFn m_layout;
    ProtocolHandler::AssetLookupFn m_default_lookup;
    std::function<void(WebView&)> m_on_destroy;
    std::vector<Window::ListenerId> m_listeners;
    Window::ListenerId m_hook = 0;
    bool m_minimized = false;

    WebView* m_hover = nullptr;
    WebView* m_capture = nullptr;

    std::optional<SleepPolicy> m_sleep_policy;
    UiTimer m_sleep_timer;
    std::vector<std::uint64_t> m_sleep_exempt;
    struct SleepingEntry {
        std::uint64_t id = 0;
        std::chrono::steady_clock::time_point hidden_since{}; ///< WebView::hidden_since() when it was put to sleep
    };
    void forget_sleeping(std::uint64_t id) noexcept;
    std::vector<SleepingEntry> m_sleeping;
    std::function<void(WebView&)> m_on_slept;
};

} // namespace boson::core
