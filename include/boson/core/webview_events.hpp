#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace boson::webview {
class BasicAuthRequest;
class ClientCertificateRequest;
class ServerCertificateError;
} // namespace boson::webview

namespace boson::core {

class WebView;

struct NavigationStartingEvent {
    std::string uri;
    bool is_user_initiated = false;
    bool is_redirected = false;
    std::uint64_t navigation_id = 0;
    bool cancel = false; ///< Set to true to cancel the navigation (URL allow-lists).
};

struct NavigationCompletedEvent {
    bool is_success = false;
    int web_error_status = 0; ///< COREWEBVIEW2_WEB_ERROR_STATUS
    int http_status_code = 0; ///< 0 when the runtime does not report it.
    std::uint64_t navigation_id = 0;
};

enum class ProcessFailedKind {
    BrowserProcessExited,
    RenderProcessExited,
    RenderProcessUnresponsive,
    FrameRenderProcessExited,
    UtilityProcessExited,
    SandboxHelperProcessExited,
    GpuProcessExited,
    PpapiPluginProcessExited,
    PpapiBrokerProcessExited,
    UnknownProcessExited,
};

struct ProcessFailedEvent {
    ProcessFailedKind kind = ProcessFailedKind::UnknownProcessExited;
    int reason = 0;     ///< COREWEBVIEW2_PROCESS_FAILED_REASON
    int exit_code = 0;
    std::string process_description;
};

enum class PermissionKind {
    Unknown,
    Microphone,
    Camera,
    Geolocation,
    Notifications,
    OtherSensors,
    ClipboardRead,
    MultipleAutomaticDownloads,
    FileReadWrite,
    Autoplay,
    LocalFonts,
    MidiSystemExclusiveMessages,
    WindowManagement,
};

enum class PermissionState {
    Default,
    Allow,
    Deny,
};

/// @brief PermissionRequested arguments. Answer synchronously with set_state(), or call defer() and
/// complete the returned function later (on the UI thread) to show your own prompt.
class PermissionRequest {
public:
    explicit PermissionRequest(Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs> args);

    [[nodiscard]] const std::string& uri() const noexcept { return m_uri; }
    [[nodiscard]] PermissionKind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool is_user_initiated() const noexcept { return m_user_initiated; }

    bool set_state(PermissionState state) const noexcept;
    /// @brief Keeps the request open; the returned function must be called exactly once.
    [[nodiscard]] std::function<void(PermissionState)> defer() const;

private:
    Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs> m_args;
    std::string m_uri;
    PermissionKind m_kind = PermissionKind::Unknown;
    bool m_user_initiated = false;
};

/// @brief Completes a deferred NewWindowRequested. If it is destroyed without being completed, the request
/// completes with the default behaviour (a popup window), so a dropped deferral never hangs the page.
class NewWindowDeferral {
public:
    NewWindowDeferral() = default;
    NewWindowDeferral(
        Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args,
        Microsoft::WRL::ComPtr<ICoreWebView2Deferral> deferral);
    ~NewWindowDeferral();

    NewWindowDeferral(const NewWindowDeferral&) = delete;
    NewWindowDeferral& operator=(const NewWindowDeferral&) = delete;
    NewWindowDeferral(NewWindowDeferral&& other) noexcept;
    NewWindowDeferral& operator=(NewWindowDeferral&& other) noexcept;

    /// @brief Opens the new window's content in target. target must be ready, share this WebView's
    /// Environment and must not have navigated yet (create it with an empty initial_url).
    bool complete_with(WebView& target);
    /// @brief Marks the request handled without opening anything (for example after navigating a tab).
    bool complete_handled();
    /// @brief Lets WebView2 open its default popup window.
    bool complete_default();

    [[nodiscard]] bool pending() const noexcept { return m_deferral != nullptr; }

private:
    bool finish() noexcept;

    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventArgs> m_args;
    Microsoft::WRL::ComPtr<ICoreWebView2Deferral> m_deferral;
};

class NewWindowRequest {
public:
    explicit NewWindowRequest(Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args);

    [[nodiscard]] const std::string& uri() const noexcept { return m_uri; }
    [[nodiscard]] bool is_user_initiated() const noexcept { return m_user_initiated; }

    /// @brief Suppresses the popup; the app handles the URI itself (for example in an existing tab).
    bool set_handled() const noexcept;
    /// @brief Synchronously redirects into an already-ready WebView that has not navigated yet.
    bool set_new_window(WebView& target) const;
    /// @brief Defers the decision, typically while a new tab WebView is being created.
    [[nodiscard]] NewWindowDeferral defer() const;

private:
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventArgs> m_args;
    std::string m_uri;
    bool m_user_initiated = false;
};

class DownloadStartingEvent {
public:
    explicit DownloadStartingEvent(Microsoft::WRL::ComPtr<ICoreWebView2DownloadStartingEventArgs> args);

    [[nodiscard]] const std::string& uri() const noexcept { return m_uri; }
    [[nodiscard]] const std::string& mime_type() const noexcept { return m_mime_type; }
    [[nodiscard]] std::optional<std::int64_t> total_bytes() const noexcept { return m_total_bytes; }
    [[nodiscard]] std::wstring result_file_path() const;

    bool set_result_file_path(std::wstring_view path) const noexcept;
    bool cancel() const noexcept;
    /// @brief true hides the default download UI.
    bool set_handled(bool handled = true) const noexcept;

private:
    Microsoft::WRL::ComPtr<ICoreWebView2DownloadStartingEventArgs> m_args;
    Microsoft::WRL::ComPtr<ICoreWebView2DownloadOperation> m_operation;
    std::string m_uri;
    std::string m_mime_type;
    std::optional<std::int64_t> m_total_bytes;
};

/// @brief Content and lifecycle callbacks. Every callback is invoked on the UI thread, from inside the
/// WebView2 event, so keep them short; long work belongs on the TaskRunner.
struct WebViewEventHandlers {
    std::function<void(std::string_view title)> on_title_changed;
    std::function<void(std::string_view uri, bool is_new_document)> on_source_changed;
    std::function<void(bool can_go_back, bool can_go_forward)> on_history_changed;
    std::function<void(NavigationStartingEvent&)> on_navigation_starting;
    std::function<void(const NavigationCompletedEvent&)> on_navigation_completed;
    std::function<void(std::string_view favicon_uri)> on_favicon_changed;
    std::function<void(NewWindowRequest&)> on_new_window_requested;
    std::function<void(bool contains_fullscreen_element)> on_fullscreen_changed;
    std::function<void(bool playing)> on_audio_changed;
    std::function<void(bool muted)> on_muted_changed;
    std::function<void(const ProcessFailedEvent&)> on_process_failed;
    std::function<void()> on_window_close_requested;
    std::function<void(PermissionRequest&)> on_permission_requested;
    std::function<void(DownloadStartingEvent&)> on_download_starting;
    /// Composition hosting only: the CSS app-region layout changed.
    std::function<void()> on_non_client_region_changed;
    /// Security prompts (boson/webview/security_events.hpp). Unset = the runtime's default UI.
    std::function<void(webview::BasicAuthRequest&)> on_basic_auth_requested;
    std::function<void(webview::ClientCertificateRequest&)> on_client_certificate_requested;
    std::function<void(webview::ServerCertificateError&)> on_server_certificate_error;
};

} // namespace boson::core
