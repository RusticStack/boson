#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/core/webview_host.hpp>
#include <boson/menu/accelerator.hpp>
#include <boson/webview/cdp.hpp>
#include <boson/webview/element.hpp>
#include <boson/webview/find.hpp>
#include <boson/webview/security_events.hpp>

#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace boson::webview {

// ---- Options -----------------------------------------------------------------------------------------------

/// @brief Where a guest keeps cookies, storage and cache (Electron's partition attribute).
struct GuestPartition {
    std::wstring profile_name; ///< Empty = the Environment's default profile.
    bool in_private = false;   ///< Nothing is written to disk; state lives until the last WebView of it closes.
};

/// @brief "" = default profile, "persist:<name>" = named persistent profile, "<name>" = named in-memory
/// (InPrivate) profile, "inprivate" = anonymous in-memory profile. nullopt for an invalid profile name.
[[nodiscard]] std::optional<GuestPartition> parse_partition(std::string_view partition);

enum class PopupPolicy {
    Deny,   ///< window.open / target=_blank are blocked; "new-window" still reports them.
    Guest,  ///< The popup opens in a new hidden guest the host adopts (<boson-webview guestinstance=id>).
    Window, ///< WebView2's own popup window.
};

/// @brief How a guest answers a prompt (permission, HTTP auth, certificate).
enum class PromptPolicy {
    Default, ///< WebView2's own UI.
    Deny,    ///< Refuse / cancel without asking.
    Allow,   ///< Permissions: allow; certificate errors: proceed. Ignored for auth prompts.
    Ask,     ///< Defer and raise an event; the host answers with Guest::respond_*.
};

[[nodiscard]] std::optional<PopupPolicy> parse_popup_policy(std::string_view name) noexcept;
[[nodiscard]] std::optional<PromptPolicy> parse_prompt_policy(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(PopupPolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(PromptPolicy policy) noexcept;

/// @brief Electron <webview> attributes and Boson's guest policies.
struct GuestOptions {
    std::string src;                  ///< First URL; empty = do not navigate (popup targets).
    std::string partition;            ///< See parse_partition.
    std::string preload_script;       ///< JS source, runs before page scripts in every guest document.
    std::string user_agent;           ///< Empty = runtime default.
    double zoom_factor = 1.0;
    PopupPolicy popups = PopupPolicy::Deny;
    PromptPolicy permissions = PromptPolicy::Default;
    PromptPolicy basic_auth = PromptPolicy::Default;
    PromptPolicy certificate_errors = PromptPolicy::Deny;
    PromptPolicy client_certificates = PromptPolicy::Default;
    bool devtools = true;
    bool context_menus = true;
    /// true: no runtime download flyout; downloads are reported as guest:download-* events only.
    bool hide_download_ui = true;
    std::wstring download_folder;     ///< Empty = the profile's default download folder.
    /// Runtime.consoleAPICalled + Runtime.exceptionThrown as console-message (enables the CDP Runtime domain).
    bool console_messages = false;
    /// Key chords swallowed inside the guest (reported to the host only), e.g. Ctrl+W, Ctrl+T, F5.
    std::vector<menu::Accelerator> reserved_accelerators;
    /// Forward Ctrl / Alt chords and F-keys typed in the guest to the host page as guest:accelerator.
    bool forward_accelerators = true;
    std::wstring script_locale;
};

enum class FocusDirection {
    Programmatic, ///< Keep the guest's focused element.
    Next,         ///< Tab into the guest: its first focusable element.
    Previous,     ///< Shift+Tab into the guest: its last focusable element.
};

enum class StopFindAction {
    ClearSelection,
    KeepSelection,
    ActivateSelection,
};

// ---- Events (JSON payloads of the guest:* bridge events; "id" is the guest id) ------------------------------

struct GuestDidStartLoading {
    static constexpr std::string_view kEvent = "guest:did-start-loading";
    std::uint64_t id = 0;
    std::string url;
    bool is_user_initiated = false;
};

struct GuestDidNavigate {
    static constexpr std::string_view kEvent = "guest:did-navigate";
    std::uint64_t id = 0;
    std::string url;
    std::int32_t http_response_code = 0;
    bool in_page = false;
    bool can_go_back = false;
    bool can_go_forward = false;
};

struct GuestDidNavigateInPage {
    static constexpr std::string_view kEvent = "guest:did-navigate-in-page";
    std::uint64_t id = 0;
    std::string url;
    bool is_main_frame = true;
};

struct GuestDomReady {
    static constexpr std::string_view kEvent = "guest:dom-ready";
    std::uint64_t id = 0;
};

struct GuestDidFinishLoad {
    static constexpr std::string_view kEvent = "guest:did-finish-load";
    std::uint64_t id = 0;
    std::string url;
};

struct GuestDidFailLoad {
    static constexpr std::string_view kEvent = "guest:did-fail-load";
    std::uint64_t id = 0;
    std::string url;
    std::int32_t error_code = 0;        ///< COREWEBVIEW2_WEB_ERROR_STATUS
    std::string error_description;
    std::int32_t http_response_code = 0;
};

struct GuestTitleUpdated {
    static constexpr std::string_view kEvent = "guest:page-title-updated";
    std::uint64_t id = 0;
    std::string title;
};

struct GuestFaviconUpdated {
    static constexpr std::string_view kEvent = "guest:page-favicon-updated";
    std::uint64_t id = 0;
    std::vector<std::string> favicons;
};

struct GuestNewWindow {
    static constexpr std::string_view kEvent = "guest:new-window";
    std::uint64_t id = 0;
    std::string url;
    std::string disposition;          ///< "denied" | "guest" | "window"
    std::uint64_t new_guest_id = 0;   ///< disposition "guest": adopt it with <boson-webview guestinstance=...>
    bool is_user_initiated = false;
};

struct GuestConsoleMessage {
    static constexpr std::string_view kEvent = "guest:console-message";
    std::uint64_t id = 0;
    std::int32_t level = 0;           ///< Electron levels: 0 verbose, 1 info, 2 warning, 3 error.
    std::string message;
    std::int32_t line = 0;
    std::string source_id;
};

struct GuestIpcMessage {
    static constexpr std::string_view kEvent = "guest:ipc-message";
    std::uint64_t id = 0;
    std::string channel;
    glz::raw_json args{"[]"};         ///< JSON array of the arguments bosonGuest.sendToHost was called with.
};

struct GuestCrashed {
    static constexpr std::string_view kEvent = "guest:crashed";
    std::uint64_t id = 0;
    std::string kind;                 ///< "render-process-exited" | "render-process-unresponsive" | ...
    std::int32_t reason = 0;          ///< COREWEBVIEW2_PROCESS_FAILED_REASON
    std::int32_t exit_code = 0;
};

struct GuestClose {
    static constexpr std::string_view kEvent = "guest:close";
    std::uint64_t id = 0;
};

struct GuestAudioState {
    static constexpr std::string_view kEvent = "guest:audio-state";
    std::uint64_t id = 0;
    bool audible = false;
    bool muted = false;
};

/// guest:download-started / guest:download-updated / guest:download-done (by phase).
struct GuestDownload {
    std::uint64_t id = 0;
    std::string phase;                ///< "started" | "updated" | "done"
    std::uint64_t download_id = 0;
    std::string url;
    std::string mime_type;
    std::string path;
    std::int64_t total_bytes = -1;    ///< -1 when unknown.
    std::int64_t received_bytes = 0;
    std::string state;                ///< "progressing" | "completed" | "interrupted" | "cancelled"
    std::string interrupt_reason;
    bool paused = false;
    bool can_resume = false;
};


struct GuestPermissionRequest {
    static constexpr std::string_view kEvent = "guest:permission-request";
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    std::string permission;           ///< "camera" | "microphone" | "geolocation" | "notifications" | ...
    std::string origin;
    bool is_user_initiated = false;
};

struct GuestLoginRequest {
    static constexpr std::string_view kEvent = "guest:login";
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    std::string url;
    std::string challenge;
};

struct GuestCertificateError {
    static constexpr std::string_view kEvent = "guest:certificate-error";
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;     ///< 0 when the policy already decided (not "ask").
    std::string url;
    std::int32_t error_code = 0;      ///< COREWEBVIEW2_WEB_ERROR_STATUS
    std::string subject;
    std::string issuer;
    std::string action;               ///< "ask" | "cancel" | "allow" | "default"
};

struct GuestClientCertificateInfo {
    std::string subject;
    std::string issuer;
    std::string display_name;
};

struct GuestSelectClientCertificate {
    static constexpr std::string_view kEvent = "guest:select-client-certificate";
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    std::string host;
    std::int32_t port = 0;
    std::vector<GuestClientCertificateInfo> certificates;
};

struct GuestFocusOut {
    static constexpr std::string_view kEvent = "guest:focus-out";
    std::uint64_t id = 0;
    std::string direction;            ///< "next" | "previous"
};

struct GuestFocus {
    static constexpr std::string_view kEvent = "guest:focus";
    std::uint64_t id = 0;
    bool focused = false;
};

/// @brief A key chord typed in the guest (Ctrl / Alt chords, F-keys), re-dispatched as a keydown on the element.
struct GuestAccelerator {
    static constexpr std::string_view kEvent = "guest:accelerator";
    std::uint64_t id = 0;
    std::string key;                  ///< DOM KeyboardEvent.key ("t", "F5", "Tab", "ArrowLeft", ...)
    std::string code;                 ///< DOM KeyboardEvent.code ("KeyT", "F5", "Digit1", ...)
    std::uint32_t key_code = 0;       ///< Win32 virtual key
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    bool repeat = false;
    bool reserved = false;            ///< true: swallowed in the guest (GuestOptions::reserved_accelerators).
};

struct GuestFoundInPage {
    static constexpr std::string_view kEvent = "guest:found-in-page";
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    std::int32_t matches = 0;
    std::int32_t active_match_ordinal = 0;
    bool final_update = true;
};

struct GuestCdpEvent {
    static constexpr std::string_view kEvent = "guest:cdp-event";
    std::uint64_t id = 0;
    std::string method;
    glz::raw_json params{"{}"};
    std::string session_id;
};

struct GuestCreated {
    static constexpr std::string_view kEvent = "guest:created";
    std::uint64_t id = 0;
    std::uint64_t opener_id = 0;
    std::string url;
    std::string reason;               ///< "element" | "popup" | "cdp" | "native"
};

struct GuestDestroyed {
    static constexpr std::string_view kEvent = "guest:destroyed";
    std::uint64_t id = 0;
};

using GuestEvent = std::variant<
    GuestDidStartLoading,
    GuestDidNavigate,
    GuestDidNavigateInPage,
    GuestDomReady,
    GuestDidFinishLoad,
    GuestDidFailLoad,
    GuestTitleUpdated,
    GuestFaviconUpdated,
    GuestNewWindow,
    GuestConsoleMessage,
    GuestIpcMessage,
    GuestCrashed,
    GuestClose,
    GuestAudioState,
    GuestDownload,
    GuestPermissionRequest,
    GuestLoginRequest,
    GuestCertificateError,
    GuestSelectClientCertificate,
    GuestFocusOut,
    GuestFocus,
    GuestAccelerator,
    GuestFoundInPage,
    GuestCdpEvent,
    GuestCreated,
    GuestDestroyed>;

/// @brief Bridge event name of a guest event ("guest:did-navigate", ...).
template <typename E>
[[nodiscard]] constexpr std::string_view guest_event_name(const E&) noexcept {
    return E::kEvent;
}

[[nodiscard]] inline std::string_view guest_event_name(const GuestDownload& e) noexcept {
    if (e.phase == "started") {
        return "guest:download-started";
    }
    return e.phase == "done" ? std::string_view("guest:download-done") : std::string_view("guest:download-updated");
}

[[nodiscard]] inline std::string_view guest_event_name(const GuestEvent& event) noexcept {
    return std::visit([](const auto& e) { return guest_event_name(e); }, event);
}

/// @brief DOM KeyboardEvent.key / .code for a Win32 virtual key (empty when unknown). shift selects the
/// shifted printable character for letters.
[[nodiscard]] std::string dom_key_of(UINT vk, bool shift);
[[nodiscard]] std::string dom_code_of(UINT vk);
/// @brief true for a key that the guest forwards to the host: any Ctrl / Alt chord, F1-F24, Escape with a
/// modifier. Plain typing never leaves the guest.
[[nodiscard]] bool is_forwarded_accelerator(UINT vk, menu::KeyModifiers modifiers) noexcept;

class GuestManager;

// ---- Guest ---------------------------------------------------------------------------------------------------

/// @brief One guest WebView: the native side of a <boson-webview> element (Electron's <webview>).
///
/// Created by GuestManager. All members must be called on the UI thread; every event is raised through
/// GuestManager listeners on the UI thread.
class Guest {
public:
    Guest(GuestManager& manager, core::WebView& webview, std::uint64_t owner_id, GuestOptions options, std::uint64_t opener_id);
    ~Guest();

    Guest(const Guest&) = delete;
    Guest& operator=(const Guest&) = delete;
    Guest(Guest&&) = delete;
    Guest& operator=(Guest&&) = delete;

    /// @brief The guest id: its WebView::id() (Electron's getWebContentsId()).
    [[nodiscard]] std::uint64_t id() const noexcept { return m_webview.id(); }
    /// @brief WebView::id() of the host page that owns the element (0 = owned by C++).
    [[nodiscard]] std::uint64_t owner_id() const noexcept { return m_owner_id; }
    void set_owner_id(std::uint64_t owner_id) noexcept { m_owner_id = owner_id; }
    [[nodiscard]] std::uint64_t opener_id() const noexcept { return m_opener_id; }
    [[nodiscard]] core::WebView& webview() const noexcept { return m_webview; }
    [[nodiscard]] const GuestOptions& options() const noexcept { return m_options; }
    /// @brief Policies (popups, prompts, accelerators) are read when the event happens, so edits apply at once.
    [[nodiscard]] GuestOptions& options() noexcept { return m_options; }
    [[nodiscard]] bool is_ready() const noexcept { return m_webview.is_ready(); }
    /// @brief Runs fn once the guest's WebView is ready (immediately when it already is).
    void when_ready(std::function<void()> fn);
    /// @brief CDP of this guest (shared with console-message and JS cdp_send).
    [[nodiscard]] Cdp& cdp() noexcept { return m_cdp; }

    // ---- Layout -----------------------------------------------------------------------------------------
    /// @brief Moves, sizes, clips (window region on the container HWND) and shows / hides the guest.
    /// Unchanged placements are skipped.
    void place(const ElementPlacement& placement);
    [[nodiscard]] const ElementPlacement& placement() const noexcept { return m_placement; }
    [[nodiscard]] std::int32_t z() const noexcept { return m_z; }
    void set_z(std::int32_t z) noexcept { m_z = z; }
    /// @brief Moves native keyboard focus into the guest.
    bool focus(FocusDirection direction = FocusDirection::Programmatic) noexcept;

    // ---- Navigation and page control ---------------------------------------------------------------------
    bool load_url(std::string_view url);
    bool go_back() noexcept { return m_webview.go_back(); }
    bool go_forward() noexcept { return m_webview.go_forward(); }
    bool reload() noexcept { return m_webview.reload(); }
    bool stop() noexcept { return m_webview.stop(); }
    bool set_zoom_factor(double factor) noexcept;
    bool set_audio_muted(bool muted) noexcept { return m_webview.set_muted(muted); }
    bool set_user_agent(std::string_view user_agent);
    bool open_devtools() noexcept { return m_webview.open_devtools(); }
    [[nodiscard]] bool is_loading() const noexcept { return m_loading; }
    [[nodiscard]] bool is_crashed() const noexcept { return m_crashed; }

    /// @brief Electron executeJavaScript: Runtime.evaluate with awaitPromise and returnByValue. done gets
    /// (true, JSON value) or (false, error message) on the UI thread.
    bool execute_javascript(std::string_view code, bool user_gesture, std::function<void(bool ok, std::string json)> done);
    /// @brief Adds a <style> to the current document (and every later one); done gets the key for
    /// remove_inserted_css.
    bool insert_css(std::string_view css, std::function<void(bool ok, std::string key)> done);
    bool remove_inserted_css(std::string_view key);
    /// @brief Host-to-guest message: bosonGuest.on(channel, ...args) listeners in the guest. args_json must
    /// be a JSON array.
    bool send(std::string_view channel, std::string_view args_json) const;
    /// @brief Starts or continues a find; results arrive as GuestFoundInPage with the returned request id
    /// (0 = not supported or failed).
    std::uint64_t find_in_page(std::string_view text, bool forward, bool match_case, bool find_next);
    bool stop_find_in_page(StopFindAction action);
    /// @brief Turns console-message reporting on or off (CDP Runtime domain).
    void set_console_messages(bool enabled);

    // ---- Pending decisions ("ask" policies) -------------------------------------------------------------
    bool respond_permission(std::uint64_t request_id, core::PermissionState state);
    bool respond_login(std::uint64_t request_id, std::optional<Credentials> credentials);
    bool respond_certificate_error(std::uint64_t request_id, ServerCertificateErrorAction action);
    /// @brief index into GuestSelectClientCertificate::certificates; nullopt = continue without one.
    bool respond_client_certificate(std::uint64_t request_id, std::optional<std::size_t> index, bool cancel = false);
    [[nodiscard]] std::size_t pending_requests() const noexcept;

    // ---- Downloads ----------------------------------------------------------------------------------------
    bool download_cancel(std::uint64_t download_id);
    bool download_pause(std::uint64_t download_id);
    bool download_resume(std::uint64_t download_id);
    [[nodiscard]] std::size_t active_downloads() const noexcept { return m_downloads.size(); }

    // ---- JS CDP relay -------------------------------------------------------------------------------------
    /// @brief Forwards these events to the owner as guest:cdp-event (already-subscribed events are skipped).
    void cdp_subscribe(std::span<const std::string> events);
    void cdp_unsubscribe(std::span<const std::string> events);

    /// @brief window.bosonGuest runtime injected into every guest document before the preload script.
    [[nodiscard]] static std::wstring guest_runtime_script();

private:
    friend class GuestManager;

    void on_ready();
    void bind_events();
    void unbind_native() noexcept;
    void emit(GuestEvent event);
    void handle_new_window(core::NewWindowRequest& request);
    bool handle_web_message(const core::WebView::WebMessage& message);
    void handle_download(ICoreWebView2DownloadStartingEventArgs* args);
    void report_download(std::uint64_t download_id, int kind);
    void apply_region() noexcept;
    void cancel_pending() noexcept;

    GuestManager& m_manager;
    core::WebView& m_webview;
    std::uint64_t m_owner_id = 0;
    std::uint64_t m_opener_id = 0;
    GuestOptions m_options;
    Cdp m_cdp;
    Cdp::SubscriptionId m_console_sub = 0;
    Cdp::SubscriptionId m_exception_sub = 0;
    std::vector<std::pair<std::string, Cdp::SubscriptionId>> m_js_cdp_subs;

    ElementPlacement m_placement;
    bool m_placed = false;
    std::int32_t m_z = 0;
    bool m_loading = false;
    bool m_crashed = false;
    std::string m_last_url;

    Microsoft::WRL::ComPtr<ICoreWebView2> m_bound;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_bound_controller;
    EventRegistrationToken m_download_token{};
    EventRegistrationToken m_dom_token{};
    EventRegistrationToken m_accel_token{};
    EventRegistrationToken m_move_focus_token{};
    EventRegistrationToken m_got_focus_token{};
    EventRegistrationToken m_lost_focus_token{};
    core::WebView::HookId m_message_hook = 0;

    std::unique_ptr<FindSession> m_find;
    std::uint64_t m_find_request = 0;
    std::string m_find_text;
    std::uint64_t m_next_css_key = 1;

    struct Pending {
        std::uint64_t request_id = 0;
        std::function<void(core::PermissionState)> permission;
        std::function<void(std::optional<Credentials>)> login;
        std::function<void(ServerCertificateErrorAction)> certificate;
        std::function<void(ClientCertificateAction, std::size_t)> client_certificate;
    };
    std::vector<Pending> m_pending;

    struct Download {
        std::uint64_t download_id = 0;
        Microsoft::WRL::ComPtr<ICoreWebView2DownloadOperation> operation;
        EventRegistrationToken bytes_token{};
        EventRegistrationToken state_token{};
        std::string url;
        std::string mime_type;
        std::chrono::steady_clock::time_point last_report{};
    };
    std::vector<Download> m_downloads;
    std::vector<std::function<void()>> m_ready_once; ///< Run once after the next on_ready (popup deferrals).
    std::shared_ptr<Guest*> m_destroy_slot;          ///< Cleared on destroy so a late on_ready finds no guest.
    bool m_stacked = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// ---- GuestManager ----------------------------------------------------------------------------------------------

struct GuestManagerOptions {
    /// Resolves a preload URL from the element's preload attribute (for example boson://app/preload.js) to
    /// script source. Unset: boson://app/* URLs are read from the host's asset lookup; others are rejected.
    std::function<std::optional<std::string>(std::string_view url)> resolve_preload;
    /// Upper bound of guests per owner (0 = unlimited); create() fails beyond it.
    std::size_t max_guests_per_owner = 64;
};

/// @brief Owns the guest WebViews of one WebViewHost (one window) and routes their events to listeners.
///
/// Guests are ordinary WebViews of the host (windowed child HWNDs, or composition visuals), created hidden
/// and shown by the first layout. They are never attached to a Bridge: guest content cannot call host
/// controllers; it only reaches the host through window.bosonGuest.sendToHost (guest:ipc-message). UI thread.
class GuestManager {
public:
    using Listener = std::function<void(Guest& guest, const GuestEvent& event)>;
    using ListenerId = std::uint64_t;

    explicit GuestManager(core::WebViewHost& host, GuestManagerOptions options = {});
    ~GuestManager();

    GuestManager(const GuestManager&) = delete;
    GuestManager& operator=(const GuestManager&) = delete;
    GuestManager(GuestManager&&) = delete;
    GuestManager& operator=(GuestManager&&) = delete;

    /// @brief Creates a hidden guest owned by owner_id (the host page's WebView::id(), or 0). Returns null for
    /// an invalid partition or when the owner reached max_guests_per_owner.
    Guest* create(std::uint64_t owner_id, GuestOptions options, std::string_view reason = "native", std::uint64_t opener_id = 0);
    /// @brief Destroys the guest (its WebView is closed on the next message-loop turn).
    bool destroy(std::uint64_t guest_id);
    /// @brief Destroys every guest of owner_id; returns how many.
    std::size_t destroy_owned_by(std::uint64_t owner_id);
    void destroy_all();

    [[nodiscard]] Guest* find(std::uint64_t guest_id) const noexcept;
    [[nodiscard]] std::vector<Guest*> guests() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_guests.size(); }

    /// @brief Applies one animation frame of element measurements (frontend/src/webview_element.ts) in a
    /// single pass: bounds + clip + visibility per guest, then z-order when it changed. Guests not owned by
    /// owner_id are ignored. Returns how many guests changed.
    std::size_t apply_layout(std::uint64_t owner_id, std::span<const GuestLayoutItem> items, double device_pixel_ratio);

    ListenerId add_listener(Listener listener);
    bool remove_listener(ListenerId id) noexcept;

    /// @brief Script source for a preload URL (see GuestManagerOptions::resolve_preload).
    [[nodiscard]] std::optional<std::string> resolve_preload(std::string_view url) const;

    [[nodiscard]] core::WebViewHost& host() const noexcept { return m_host; }

    /// @brief Raises event to every listener (UI thread). Used by Guest.
    void emit(Guest& guest, const GuestEvent& event);

private:
    void restack(std::uint64_t owner_id);

    core::WebViewHost& m_host;
    GuestManagerOptions m_options;
    std::vector<std::unique_ptr<Guest>> m_guests;
    struct ListenerEntry {
        ListenerId id = 0;
        std::shared_ptr<Listener> fn;
    };
    std::vector<ListenerEntry> m_listeners;
    ListenerId m_next_listener = 1;
};

} // namespace boson::webview

