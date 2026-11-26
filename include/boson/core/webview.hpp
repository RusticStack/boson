#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <dcomp.h>

#include <boson/core/environment.hpp>
#include <boson/core/protocol.hpp>
#include <boson/core/webview_events.hpp>
#include <boson/exec/callback_sender.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::core {

class CompositionHost;

enum class HostingMode {
    /// Each WebView is a child HWND (inside a Boson container window, for z-order). Default; works with
    /// hardware DRM and MPO.
    Windowed,
    /// ICoreWebView2CompositionController rendering into a DirectComposition visual tree, so WebViews can
    /// overlap with opacity and rounded clips. Input is forwarded by WebViewHost.
    Composition,
};

enum class MemoryUsageTarget {
    Normal,
    Low,
};

/// @brief ICoreWebView2Settings toggles. Unset fields keep the runtime default.
struct WebViewSettings {
    std::optional<bool> default_context_menus_enabled;
    std::optional<bool> devtools_enabled;
    std::optional<bool> zoom_control_enabled;
    std::optional<bool> browser_accelerator_keys_enabled; ///< ICoreWebView2Settings3
    std::optional<bool> password_autosave_enabled;        ///< ICoreWebView2Settings4
    std::optional<bool> general_autofill_enabled;         ///< ICoreWebView2Settings4
    std::optional<bool> pinch_zoom_enabled;               ///< ICoreWebView2Settings5
    std::optional<bool> swipe_navigation_enabled;         ///< ICoreWebView2Settings6
    std::optional<bool> status_bar_enabled;
    std::optional<bool> script_dialogs_enabled;
    /// chrome.webview.postMessage availability. Turn off for third-party content WebViews.
    std::optional<bool> web_messages_enabled;
    /// CSS app-region: drag / no-drag (ICoreWebView2Settings9).
    std::optional<bool> non_client_region_support_enabled;
    std::optional<std::wstring> user_agent;               ///< ICoreWebView2Settings2
};

struct WebViewConfig {
    /// Used only when the WebView creates its own Environment (single-WebView apps).
    std::wstring user_data_folder;
    EnvironmentConfig environment;

    std::optional<std::string> dev_url;
    /// First navigation. Empty means "do not navigate", which a NewWindowRequested target requires.
    std::string initial_url = "boson://app/index.html";
    bool transparent_background = false;
    /// Serve boson://app/* from the AssetLookupFn. Off for third-party content WebViews.
    bool serve_boson_assets = true;
    /// Extra "Name: value" header lines (CRLF-separated) on every boson://app response, e.g. a CSP.
    std::string asset_response_headers;
    bool start_visible = true;
    HostingMode hosting = HostingMode::Windowed;
    /// Required for HostingMode::Composition (WebViewHost supplies it).
    std::shared_ptr<CompositionHost> composition;
    WebViewSettings settings;
    /// Injected with AddScriptToExecuteOnDocumentCreated before the first navigation.
    std::vector<std::wstring> scripts_on_document_created;
    /// Named profile (ICoreWebView2ControllerOptions, own cookies/storage/cache). Empty = default profile.
    std::wstring profile_name;
    /// InPrivate profile: nothing is persisted to the user data folder.
    bool in_private = false;
    /// BCP 47 locale for JavaScript Intl / Date formatting (ICoreWebView2ControllerOptions2::ScriptLocale).
    std::wstring script_locale;
};

/// @brief One ICoreWebView2Controller. All members must be called on the UI thread.
class WebView {
public:
    using MessageCallback = std::function<void(std::string_view json_payload, std::string_view source_uri)>;
    using ReadyCallback = std::function<void()>;

    /// @brief A raw WebMessageReceived event as seen by observers. args stays valid only during the call.
    struct WebMessage {
        std::string_view json;
        std::string_view source_uri;
        ICoreWebView2WebMessageReceivedEventArgs* args = nullptr;
    };
    /// Runs before the message callback; return true to consume the message (it then never reaches the Bridge).
    using WebMessageObserver = std::function<bool(const WebMessage& message)>;
    /// Runs before the boson://app asset handler; return true when it set a response (or took a deferral).
    using ResourceRequestHandler =
        std::function<bool(std::string_view uri, ICoreWebView2WebResourceRequestedEventArgs* args)>;
    using HookId = std::uint64_t;

    /// @brief Single-WebView form: creates a private Environment from config.environment on initialize().
    explicit WebView(WebViewConfig config = {});
    /// @brief Shares env with other WebViews: one browser process tree for all of them.
    WebView(std::shared_ptr<Environment> env, WebViewConfig config = {});
    ~WebView();

    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;
    WebView(WebView&&) = delete;
    WebView& operator=(WebView&&) = delete;

    bool initialize(
        HWND parent,
        ProtocolHandler::AssetLookupFn asset_lookup,
        MessageCallback on_message,
        ReadyCallback on_ready = {});

    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    [[nodiscard]] bool is_ready() const noexcept { return m_webview != nullptr; }
    [[nodiscard]] ICoreWebView2* core_webview() const noexcept { return m_webview.Get(); }
    [[nodiscard]] ICoreWebView2Controller* controller() const noexcept { return m_controller.Get(); }
    [[nodiscard]] ICoreWebView2CompositionController* composition_controller() const noexcept {
        return m_composition_controller.Get();
    }
    [[nodiscard]] const std::shared_ptr<Environment>& environment() const noexcept { return m_env; }
    [[nodiscard]] const WebViewConfig& config() const noexcept { return m_config; }
    [[nodiscard]] HWND parent_hwnd() const noexcept { return m_parent; }
    /// @brief The Boson container child window that hosts the controller (windowed hosting), else null.
    [[nodiscard]] HWND container_hwnd() const noexcept { return m_container; }
    [[nodiscard]] HostingMode hosting() const noexcept { return m_config.hosting; }
    [[nodiscard]] WebViewEventHandlers& events() noexcept { return m_events; }

    void set_message_callback(MessageCallback cb) { m_on_message = std::move(cb); }
    void set_ready_callback(ReadyCallback cb) { m_on_ready = std::move(cb); }

    /// @brief Adds a message observer that survives Environment::recreate (UI thread). Safe inside a callback.
    HookId add_web_message_observer(WebMessageObserver observer);
    bool remove_web_message_observer(HookId id) noexcept;
    /// @brief Adds a WebResourceRequested filter (e.g. L"boson://media/*") and its handler. The filter is
    /// re-registered whenever the controller is re-created.
    HookId add_resource_handler(std::wstring uri_filter, ResourceRequestHandler handler);
    bool remove_resource_handler(HookId id) noexcept;

    // ---- Layout, visibility, z-order (#3) ---------------------------------------------------------------
    /// @brief Fills {0,0,width,height} of the parent client area.
    void resize(int width, int height) noexcept;
    /// @brief Bounds in physical pixels relative to the parent client area; any origin.
    void set_bounds(RECT physical_px) noexcept;
    /// @brief Bounds in DIPs, scaled with GetDpiForWindow(parent).
    void set_bounds_dip(RECT dip) noexcept;
    [[nodiscard]] RECT bounds() const noexcept { return m_bounds; }
    /// @brief put_IsVisible; hidden WebViews are throttled and may be suspended.
    void set_visible(bool visible) noexcept;
    [[nodiscard]] bool is_visible() const noexcept { return m_visible; }
    /// @brief Hides the controller while the parent is minimized without changing is_visible().
    void set_parent_minimized(bool minimized) noexcept;
    /// @brief Keeps the controller visible (IsVisible = true) while the parent window is minimized.
    /// For media apps: a hidden controller lets the runtime pause or stall background video
    /// (see docs/COMPOSITION_DRM_SPIKE.md). The page keeps rendering while minimized.
    void set_keep_visible_when_minimized(bool keep) noexcept;
    [[nodiscard]] bool keep_visible_when_minimized() const noexcept { return m_keep_visible_when_minimized; }
    void bring_to_front() noexcept;
    void send_to_back() noexcept;
    bool focus() noexcept;
    /// @brief Composition only: whole-WebView opacity (0..1) and a rounded clip radius in physical pixels.
    bool set_opacity(float opacity) noexcept;
    bool set_corner_radius(float radius_px) noexcept;
    /// @brief Must be called whenever the top-level window moves (WebViewHost does it for you).
    void notify_parent_window_position_changed() noexcept;
    /// @brief Deterministic teardown: removes every handler and closes the controller.
    void close() noexcept;
    [[nodiscard]] bool is_closed() const noexcept { return m_closed; }

    // ---- Navigation and page control (#5) ---------------------------------------------------------------
    bool navigate(std::string_view url) const;
    bool navigate_to_string(std::string_view html) const;
    bool go_back() const noexcept;
    bool go_forward() const noexcept;
    [[nodiscard]] bool can_go_back() const noexcept;
    [[nodiscard]] bool can_go_forward() const noexcept;
    bool reload() const noexcept;
    bool stop() const noexcept;
    [[nodiscard]] std::optional<std::string> source() const;
    [[nodiscard]] std::optional<std::string> document_title() const;

    /// @brief Runs js in the top document; callback receives the JSON-encoded result on the UI thread.
    bool execute_script(std::string_view js, std::function<void(HRESULT, std::string)> callback) const;
    [[nodiscard]] exec::CallbackSender<std::string> execute_script(std::string_view js) const;
    bool add_script_on_document_created(std::string_view js, std::function<void(HRESULT, std::string)> callback) const;
    [[nodiscard]] exec::CallbackSender<std::string> add_script_on_document_created(std::string_view js) const;
    bool remove_script_on_document_created(std::string_view id) const;

    bool set_zoom_factor(double factor) const noexcept;
    [[nodiscard]] std::optional<double> zoom_factor() const noexcept;
    bool set_muted(bool muted) const noexcept;
    [[nodiscard]] std::optional<bool> is_muted() const noexcept;
    [[nodiscard]] std::optional<bool> is_playing_audio() const noexcept;
    [[nodiscard]] std::optional<bool> contains_fullscreen_element() const noexcept;
    bool open_devtools() const noexcept;
    bool open_task_manager() const noexcept;
    bool apply_settings(const WebViewSettings& settings);

    /// @brief PNG bytes of the current favicon (ICoreWebView2_15::GetFavicon).
    bool get_favicon_png(std::function<void(HRESULT, std::vector<std::byte>)> callback) const;
    [[nodiscard]] exec::CallbackSender<std::vector<std::byte>> get_favicon_png() const;

    bool set_transparent_background(bool enabled) noexcept;
    bool post_web_message_as_json(std::string_view json) const;
    bool post_web_message_as_string(std::string_view text) const;

    // ---- Suspension and memory (#7) ---------------------------------------------------------------------
    /// @brief ICoreWebView2_3::TrySuspend. Requires set_visible(false) first; result true = suspended.
    bool try_suspend(std::function<void(HRESULT, bool)> callback);
    [[nodiscard]] exec::CallbackSender<bool> try_suspend();
    bool resume() noexcept;
    [[nodiscard]] std::optional<bool> is_suspended() const noexcept;
    /// @brief Closes the controller, and with it the page's renderer process, keeping the container, bounds and
    /// configuration. The page's state is lost. set_visible(true) creates a new controller and reloads the last
    /// URL (the wake costs a page load instead of TrySuspend's ~15 ms). This is the "memory matters more than wake
    /// time" tab sleep (SleepPolicy::discard). False when there is no live controller or it is already discarded.
    bool discard();
    [[nodiscard]] bool is_discarded() const noexcept { return m_discarded; }
    /// @brief ICoreWebView2_19::put_MemoryUsageTargetLevel. set_visible(true) restores Normal.
    bool set_memory_usage_target(MemoryUsageTarget target) noexcept;
    [[nodiscard]] MemoryUsageTarget memory_usage_target() const noexcept { return m_memory_target; }
    [[nodiscard]] std::chrono::steady_clock::time_point hidden_since() const noexcept { return m_hidden_since; }
    [[nodiscard]] bool last_known_playing_audio() const noexcept { return m_playing_audio; }

    // ---- Composition input / non-client regions (#9, #10) ----------------------------------------------
    bool send_mouse_input(
        COREWEBVIEW2_MOUSE_EVENT_KIND kind,
        COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys,
        UINT32 mouse_data,
        POINT point_in_webview) const noexcept;
    bool send_pointer_input(COREWEBVIEW2_POINTER_EVENT_KIND kind, ICoreWebView2PointerInfo* info) const noexcept;
    [[nodiscard]] HCURSOR cursor() const noexcept;
    [[nodiscard]] std::optional<COREWEBVIEW2_NON_CLIENT_REGION_KIND> non_client_region_at(POINT point_in_webview) const noexcept;
    void set_cursor_changed_callback(std::function<void()> cb) { m_on_cursor_changed = std::move(cb); }

private:
    friend class Environment;

    static LRESULT CALLBACK container_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

    bool create_container();
    void create_controller(ICoreWebView2Environment* env);
    void on_controller_created(HRESULT result, ICoreWebView2Controller* controller);
    void configure_new_controller();
    void register_event_handlers();
    void unregister_event_handlers() noexcept;
    void apply_bounds() noexcept;
    bool dispatch_web_message(const WebMessage& message);
    bool dispatch_resource_request(ICoreWebView2WebResourceRequestedEventArgs* args, std::string_view uri);
    void apply_visibility() noexcept;
    void raise_caption_overlay() const noexcept;
    void release_controller() noexcept;
    void detach_from_environment() noexcept;
    void reattach_to_environment();

    static std::atomic<std::uint64_t> s_next_id;

    std::uint64_t m_id;
    WebViewConfig m_config;
    std::shared_ptr<Environment> m_env;
    HWND m_parent = nullptr;
    HWND m_container = nullptr;
    ProtocolHandler m_protocol;
    MessageCallback m_on_message;
    ReadyCallback m_on_ready;
    WebViewEventHandlers m_events;
    std::function<void()> m_on_cursor_changed;

    struct MessageObserverEntry {
        HookId id = 0;
        WebMessageObserver fn;
    };
    struct ResourceHandlerEntry {
        HookId id = 0;
        std::wstring filter;
        ResourceRequestHandler fn;
    };
    std::vector<MessageObserverEntry> m_message_observers;
    std::vector<ResourceHandlerEntry> m_resource_handlers;
    HookId m_next_hook_id = 1;
    int m_hook_dispatch_depth = 0;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2CompositionController> m_composition_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webview;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> m_visual;

    RECT m_bounds{};
    bool m_has_bounds = false;
    bool m_visible = true;
    bool m_parent_minimized = false;
    bool m_keep_visible_when_minimized = false;
    bool m_closed = false;
    bool m_creating = false;
    bool m_playing_audio = false;
    float m_opacity = 1.0f;
    float m_corner_radius = 0.0f;
    MemoryUsageTarget m_memory_target = MemoryUsageTarget::Normal;
    std::chrono::steady_clock::time_point m_hidden_since{};
    bool m_discarded = false;
    std::string m_restore_url;

    struct Tokens {
        EventRegistrationToken resource{};
        EventRegistrationToken message{};
        EventRegistrationToken title{};
        EventRegistrationToken source{};
        EventRegistrationToken history{};
        EventRegistrationToken nav_starting{};
        EventRegistrationToken nav_completed{};
        EventRegistrationToken favicon{};
        EventRegistrationToken new_window{};
        EventRegistrationToken fullscreen{};
        EventRegistrationToken audio{};
        EventRegistrationToken muted{};
        EventRegistrationToken process_failed{};
        EventRegistrationToken close_requested{};
        EventRegistrationToken permission{};
        EventRegistrationToken download{};
        EventRegistrationToken cursor{};
        EventRegistrationToken non_client{};
        EventRegistrationToken basic_auth{};
        EventRegistrationToken client_certificate{};
        EventRegistrationToken server_certificate{};
        EventRegistrationToken zoom{};
        EventRegistrationToken notification{};
    } m_tokens;

    std::shared_ptr<std::atomic<bool>> m_alive;
};

} // namespace boson::core
