#include <boson/webview/guest.hpp>

#include <boson/core/result.hpp>
#include <boson/webview/profile.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace boson::webview::wire {

/// Guest runtime messages ({"__bosonGuest":"ipc","channel":...,"args":[...]}).
struct GuestWireMessage {
    std::string kind;
    std::string channel;
    glz::raw_json args{"[]"};
};

struct EvalRemoteObject {
    std::string type;
    std::optional<glz::raw_json> value;
    std::optional<std::string> description;
    std::optional<std::string> unserializableValue;
};

struct EvalExceptionDetails {
    std::string text;
    std::optional<EvalRemoteObject> exception;
};

struct EvalResult {
    EvalRemoteObject result;
    std::optional<EvalExceptionDetails> exceptionDetails;
};

struct ConsoleFrame {
    std::string url;
    std::int32_t lineNumber = 0;
};

struct ConsoleStack {
    std::vector<ConsoleFrame> callFrames;
};

struct ConsoleApiCalled {
    std::string type;
    std::vector<EvalRemoteObject> args;
    std::optional<ConsoleStack> stackTrace;
};

struct ExceptionThrownDetails {
    std::string text;
    std::int32_t lineNumber = 0;
    std::optional<std::string> url;
    std::optional<EvalRemoteObject> exception;
};

struct ExceptionThrown {
    ExceptionThrownDetails exceptionDetails;
};

struct CdpErrorBody {
    std::int32_t code = 0;
    std::string message;
};

} // namespace boson::webview::wire

template <>
struct glz::meta<boson::webview::wire::GuestWireMessage> {
    using T = boson::webview::wire::GuestWireMessage;
    static constexpr auto value = glz::object("__bosonGuest", &T::kind, "channel", &T::channel, "args", &T::args);
};

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr glz::opts kLenient{.error_on_unknown_keys = false};

std::string json_string(std::string_view text) {
    std::string out;
    if (glz::write_json(text, out)) {
        return "\"\"";
    }
    return out;
}

std::uint64_t next_request_id() noexcept {
    static std::uint64_t s_next = 1; // UI thread only
    return s_next++;
}

std::string_view permission_name(core::PermissionKind kind) noexcept {
    using K = core::PermissionKind;
    switch (kind) {
    case K::Microphone: return "microphone";
    case K::Camera: return "camera";
    case K::Geolocation: return "geolocation";
    case K::Notifications: return "notifications";
    case K::OtherSensors: return "sensors";
    case K::ClipboardRead: return "clipboard-read";
    case K::MultipleAutomaticDownloads: return "multiple-automatic-downloads";
    case K::FileReadWrite: return "file-system";
    case K::Autoplay: return "autoplay";
    case K::LocalFonts: return "local-fonts";
    case K::MidiSystemExclusiveMessages: return "midi-sysex";
    case K::WindowManagement: return "window-management";
    case K::Unknown: break;
    }
    return "unknown";
}

std::string_view process_failed_name(core::ProcessFailedKind kind) noexcept {
    using K = core::ProcessFailedKind;
    switch (kind) {
    case K::BrowserProcessExited: return "browser-process-exited";
    case K::RenderProcessExited: return "render-process-exited";
    case K::RenderProcessUnresponsive: return "render-process-unresponsive";
    case K::FrameRenderProcessExited: return "frame-render-process-exited";
    case K::UtilityProcessExited: return "utility-process-exited";
    case K::SandboxHelperProcessExited: return "sandbox-helper-process-exited";
    case K::GpuProcessExited: return "gpu-process-exited";
    case K::PpapiPluginProcessExited: return "ppapi-plugin-process-exited";
    case K::PpapiBrokerProcessExited: return "ppapi-broker-process-exited";
    case K::UnknownProcessExited: break;
    }
    return "unknown-process-exited";
}

std::string_view web_error_name(int status) noexcept {
    switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT: return "ERR_CERT_COMMON_NAME_INVALID";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED: return "ERR_CERT_DATE_INVALID";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS: return "ERR_BAD_SSL_CLIENT_AUTH_CERT";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED: return "ERR_CERT_REVOKED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID: return "ERR_CERT_INVALID";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE: return "ERR_ADDRESS_UNREACHABLE";
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT: return "ERR_TIMED_OUT";
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE: return "ERR_INVALID_HTTP_RESPONSE";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED: return "ERR_CONNECTION_ABORTED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET: return "ERR_CONNECTION_RESET";
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED: return "ERR_INTERNET_DISCONNECTED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT: return "ERR_CONNECTION_REFUSED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED: return "ERR_NAME_NOT_RESOLVED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED: return "ERR_ABORTED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED: return "ERR_TOO_MANY_REDIRECTS";
    case COREWEBVIEW2_WEB_ERROR_STATUS_UNEXPECTED_ERROR: return "ERR_UNEXPECTED";
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_AUTHENTICATION_CREDENTIALS_REQUIRED: return "ERR_INVALID_AUTH_CREDENTIALS";
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_PROXY_AUTHENTICATION_REQUIRED: return "ERR_PROXY_AUTH_REQUESTED";
    default: break;
    }
    return "ERR_FAILED";
}

std::string_view interrupt_reason_name(COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason) noexcept {
    switch (reason) {
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE: return "";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_FAILED: return "file-failed";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED: return "file-access-denied";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE: return "file-no-space";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NAME_TOO_LONG: return "file-name-too-long";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TOO_LARGE: return "file-too-large";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_MALICIOUS: return "file-malicious";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TRANSIENT_ERROR: return "file-transient-error";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED_BY_POLICY: return "file-blocked-by-policy";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED: return "file-security-check-failed";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TOO_SHORT: return "file-too-short";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_HASH_MISMATCH: return "file-hash-mismatch";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_FAILED: return "network-failed";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT: return "network-timeout";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED: return "network-disconnected";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_SERVER_DOWN: return "network-server-down";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_INVALID_REQUEST: return "network-invalid-request";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED: return "server-failed";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE: return "server-no-range";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT: return "server-bad-content";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED: return "server-unauthorized";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CERTIFICATE_PROBLEM: return "server-certificate-problem";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN: return "server-forbidden";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_UNEXPECTED_RESPONSE: return "server-unexpected-response";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CONTENT_LENGTH_MISMATCH: return "server-content-length-mismatch";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CROSS_ORIGIN_REDIRECT: return "server-cross-origin-redirect";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED: return "user-canceled";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_SHUTDOWN: return "user-shutdown";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED: return "user-paused";
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_DOWNLOAD_PROCESS_CRASHED: return "download-process-crashed";
    default: break;
    }
    return "unknown";
}

/// The JSON text of a CDP RemoteObject passed by value (or its description when not serializable).
std::string remote_object_text(const wire::EvalRemoteObject& object) {
    if (object.value.has_value()) {
        return object.value->str;
    }
    if (object.unserializableValue.has_value()) {
        return json_string(*object.unserializableValue);
    }
    if (object.type == "undefined") {
        return "null";
    }
    return object.description.has_value() ? json_string(*object.description) : std::string("null");
}

std::string console_text(const wire::EvalRemoteObject& arg) {
    if (arg.type == "string" && arg.value.has_value()) {
        std::string s;
        if (!glz::read_json(s, arg.value->str)) {
            return s;
        }
        return arg.value->str;
    }
    if (arg.value.has_value()) {
        return arg.value->str;
    }
    if (arg.unserializableValue.has_value()) {
        return *arg.unserializableValue;
    }
    if (arg.description.has_value()) {
        return *arg.description;
    }
    return arg.type;
}

std::int32_t console_level(std::string_view type) noexcept {
    if (type == "error" || type == "assert") {
        return 3;
    }
    if (type == "warning") {
        return 2;
    }
    if (type == "debug" || type == "trace") {
        return 0;
    }
    return 1;
}

} // namespace

// ---- Free functions -----------------------------------------------------------------------------------------

std::optional<GuestPartition> parse_partition(std::string_view partition) {
    GuestPartition out;
    if (partition.empty()) {
        return out;
    }
    if (partition == "inprivate") {
        out.in_private = true;
        return out;
    }
    constexpr std::string_view kPersist = "persist:";
    std::string_view name = partition;
    if (partition.starts_with(kPersist)) {
        name = partition.substr(kPersist.size());
    } else {
        out.in_private = true;
    }
    out.profile_name = core::utf8_to_wide(name);
    if (!is_valid_profile_name(out.profile_name)) {
        return std::nullopt;
    }
    return out;
}

std::optional<PopupPolicy> parse_popup_policy(std::string_view name) noexcept {
    if (name == "deny" || name.empty()) {
        return PopupPolicy::Deny;
    }
    if (name == "guest") {
        return PopupPolicy::Guest;
    }
    if (name == "window") {
        return PopupPolicy::Window;
    }
    return std::nullopt;
}

std::optional<PromptPolicy> parse_prompt_policy(std::string_view name) noexcept {
    if (name == "default" || name.empty()) {
        return PromptPolicy::Default;
    }
    if (name == "deny") {
        return PromptPolicy::Deny;
    }
    if (name == "allow") {
        return PromptPolicy::Allow;
    }
    if (name == "ask") {
        return PromptPolicy::Ask;
    }
    return std::nullopt;
}

std::string_view to_string(PopupPolicy policy) noexcept {
    switch (policy) {
    case PopupPolicy::Guest: return "guest";
    case PopupPolicy::Window: return "window";
    case PopupPolicy::Deny: break;
    }
    return "deny";
}

std::string_view to_string(PromptPolicy policy) noexcept {
    switch (policy) {
    case PromptPolicy::Deny: return "deny";
    case PromptPolicy::Allow: return "allow";
    case PromptPolicy::Ask: return "ask";
    case PromptPolicy::Default: break;
    }
    return "default";
}

std::string dom_key_of(UINT vk, bool shift) {
    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, shift ? static_cast<char>(vk) : static_cast<char>(vk - 'A' + 'a'));
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return std::string(1, static_cast<char>('0' + (vk - VK_NUMPAD0)));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    switch (vk) {
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_LEFT: return "ArrowLeft";
    case VK_RIGHT: return "ArrowRight";
    case VK_UP: return "ArrowUp";
    case VK_DOWN: return "ArrowDown";
    case VK_SPACE: return " ";
    case VK_APPS: return "ContextMenu";
    case VK_OEM_PLUS: return shift ? "+" : "=";
    case VK_OEM_MINUS: return shift ? "_" : "-";
    case VK_OEM_COMMA: return shift ? "<" : ",";
    case VK_OEM_PERIOD: return shift ? ">" : ".";
    case VK_OEM_1: return shift ? ":" : ";";
    case VK_OEM_2: return shift ? "?" : "/";
    case VK_OEM_3: return shift ? "~" : "\x60";
    case VK_OEM_4: return shift ? "{" : "[";
    case VK_OEM_5: return shift ? "|" : "\\";
    case VK_OEM_6: return shift ? "}" : "]";
    case VK_OEM_7: return shift ? "\"" : "'";
    case VK_ADD: return "+";
    case VK_SUBTRACT: return "-";
    case VK_MULTIPLY: return "*";
    case VK_DIVIDE: return "/";
    case VK_DECIMAL: return ".";
    default: break;
    }
    return {};
}

std::string dom_code_of(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return std::string("Key") + static_cast<char>(vk);
    }
    if (vk >= '0' && vk <= '9') {
        return std::string("Digit") + static_cast<char>(vk);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_OEM_PLUS: return "Equal";
    case VK_OEM_MINUS: return "Minus";
    case VK_OEM_COMMA: return "Comma";
    case VK_OEM_PERIOD: return "Period";
    case VK_OEM_1: return "Semicolon";
    case VK_OEM_2: return "Slash";
    case VK_OEM_3: return "Backquote";
    case VK_OEM_4: return "BracketLeft";
    case VK_OEM_5: return "Backslash";
    case VK_OEM_6: return "BracketRight";
    case VK_OEM_7: return "Quote";
    case VK_ADD: return "NumpadAdd";
    case VK_SUBTRACT: return "NumpadSubtract";
    case VK_MULTIPLY: return "NumpadMultiply";
    case VK_DIVIDE: return "NumpadDivide";
    case VK_DECIMAL: return "NumpadDecimal";
    default: break;
    }
    return dom_key_of(vk, false);
}

bool is_forwarded_accelerator(UINT vk, menu::KeyModifiers modifiers) noexcept {
    if (menu::is_modifier_key(vk)) {
        return false;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return true;
    }
    return menu::has_modifier(modifiers, menu::KeyModifiers::Ctrl) || menu::has_modifier(modifiers, menu::KeyModifiers::Alt) ||
           menu::has_modifier(modifiers, menu::KeyModifiers::Win);
}

// ---- Guest -------------------------------------------------------------------------------------------------

std::wstring Guest::guest_runtime_script() {
    return LR"JS((() => {
  'use strict';
  if (window.top !== window || Object.prototype.hasOwnProperty.call(window, 'bosonGuest')) return;
  const wv = window.chrome && window.chrome.webview;
  if (!wv) return;
  const TAG = '__bosonGuest';
  const post = wv.postMessage.bind(wv);
  const listeners = new Map();
  wv.addEventListener('message', (event) => {
    const d = event.data;
    if (!d || typeof d !== 'object' || d[TAG] !== 'msg') return;
    const set = listeners.get(d.channel);
    if (!set) return;
    const args = Array.isArray(d.args) ? d.args : [];
    for (const fn of [...set]) {
      try { fn({ channel: d.channel }, ...args); } catch (err) { console.error(err); }
    }
  });
  const api = Object.freeze({
    sendToHost(channel, ...args) { post({ [TAG]: 'ipc', channel: String(channel), args }); },
    on(channel, fn) {
      let set = listeners.get(channel);
      if (!set) { set = new Set(); listeners.set(channel, set); }
      set.add(fn);
      return () => api.off(channel, fn);
    },
    once(channel, fn) {
      const off = api.on(channel, (...a) => { off(); fn(...a); });
      return off;
    },
    off(channel, fn) {
      const set = listeners.get(channel);
      if (set) { set.delete(fn); if (set.size === 0) listeners.delete(channel); }
    },
  });
  Object.defineProperty(window, 'bosonGuest', { value: api, enumerable: false, configurable: false, writable: false });
})();)JS";
}

Guest::Guest(GuestManager& manager, core::WebView& webview, std::uint64_t owner_id, GuestOptions options, std::uint64_t opener_id)
    : m_manager(manager),
      m_webview(webview),
      m_owner_id(owner_id),
      m_opener_id(opener_id),
      m_options(std::move(options)),
      m_cdp(webview) {
    core::WebViewEventHandlers& ev = m_webview.events();
    ev.on_navigation_starting = [this](core::NavigationStartingEvent& e) {
        m_loading = true;
        m_crashed = false;
        emit(GuestDidStartLoading{.id = id(), .url = e.uri, .is_user_initiated = e.is_user_initiated});
    };
    ev.on_navigation_completed = [this](const core::NavigationCompletedEvent& e) {
        m_loading = false;
        std::string url = m_webview.source().value_or(std::string{});
        if (e.is_success) {
            m_last_url = url;
            emit(GuestDidNavigate{
                .id = id(),
                .url = url,
                .http_response_code = e.http_status_code,
                .in_page = false,
                .can_go_back = m_webview.can_go_back(),
                .can_go_forward = m_webview.can_go_forward(),
            });
            emit(GuestDidFinishLoad{.id = id(), .url = std::move(url)});
        } else {
            emit(GuestDidFailLoad{
                .id = id(),
                .url = std::move(url),
                .error_code = e.web_error_status,
                .error_description = std::string(web_error_name(e.web_error_status)),
                .http_response_code = e.http_status_code,
            });
        }
    };
    ev.on_source_changed = [this](std::string_view uri, bool is_new_document) {
        if (!is_new_document && !uri.empty() && uri != m_last_url) {
            m_last_url = std::string(uri);
            emit(GuestDidNavigateInPage{.id = id(), .url = std::string(uri), .is_main_frame = true});
        }
    };
    ev.on_title_changed = [this](std::string_view title) { emit(GuestTitleUpdated{.id = id(), .title = std::string(title)}); };
    ev.on_favicon_changed = [this](std::string_view uri) {
        GuestFaviconUpdated e{.id = id(), .favicons = {}};
        if (!uri.empty()) {
            e.favicons.emplace_back(uri);
        }
        emit(std::move(e));
    };
    ev.on_new_window_requested = [this](core::NewWindowRequest& request) { handle_new_window(request); };
    ev.on_audio_changed = [this](bool playing) {
        emit(GuestAudioState{.id = id(), .audible = playing, .muted = m_webview.is_muted().value_or(false)});
    };
    ev.on_muted_changed = [this](bool muted) {
        emit(GuestAudioState{.id = id(), .audible = m_webview.last_known_playing_audio(), .muted = muted});
    };
    ev.on_process_failed = [this](const core::ProcessFailedEvent& e) {
        using K = core::ProcessFailedKind;
        if (e.kind != K::RenderProcessExited && e.kind != K::RenderProcessUnresponsive && e.kind != K::FrameRenderProcessExited &&
            e.kind != K::BrowserProcessExited) {
            return;
        }
        m_crashed = e.kind != K::RenderProcessUnresponsive && e.kind != K::FrameRenderProcessExited;
        m_loading = false;
        emit(GuestCrashed{.id = id(), .kind = std::string(process_failed_name(e.kind)), .reason = e.reason, .exit_code = e.exit_code});
    };
    ev.on_window_close_requested = [this]() { emit(GuestClose{.id = id()}); };
    ev.on_permission_requested = [this](core::PermissionRequest& request) {
        switch (m_options.permissions) {
        case PromptPolicy::Default: return;
        case PromptPolicy::Allow: request.set_state(core::PermissionState::Allow); return;
        case PromptPolicy::Deny: request.set_state(core::PermissionState::Deny); return;
        case PromptPolicy::Ask: break;
        }
        const std::uint64_t request_id = next_request_id();
        m_pending.push_back(Pending{.request_id = request_id, .permission = request.defer(), .login = {}, .certificate = {}, .client_certificate = {}});
        emit(GuestPermissionRequest{
            .id = id(),
            .request_id = request_id,
            .permission = std::string(permission_name(request.kind())),
            .origin = request.uri(),
            .is_user_initiated = request.is_user_initiated(),
        });
    };
    ev.on_basic_auth_requested = [this](BasicAuthRequest& request) {
        switch (m_options.basic_auth) {
        case PromptPolicy::Default:
        case PromptPolicy::Allow: return;
        case PromptPolicy::Deny: request.cancel(); return;
        case PromptPolicy::Ask: break;
        }
        const std::uint64_t request_id = next_request_id();
        m_pending.push_back(Pending{.request_id = request_id, .permission = {}, .login = request.defer(), .certificate = {}, .client_certificate = {}});
        emit(GuestLoginRequest{.id = id(), .request_id = request_id, .url = request.uri(), .challenge = request.challenge()});
    };
    ev.on_server_certificate_error = [this](ServerCertificateError& error) {
        GuestCertificateError e{
            .id = id(),
            .request_id = 0,
            .url = error.request_uri(),
            .error_code = error.error_status(),
            .subject = error.certificate() ? error.certificate()->subject : std::string{},
            .issuer = error.certificate() ? error.certificate()->issuer : std::string{},
            .action = std::string(to_string(m_options.certificate_errors)),
        };
        switch (m_options.certificate_errors) {
        case PromptPolicy::Default: break;
        case PromptPolicy::Allow: error.set_action(ServerCertificateErrorAction::AlwaysAllow); break;
        case PromptPolicy::Deny:
            error.set_action(ServerCertificateErrorAction::Cancel);
            e.action = "cancel";
            break;
        case PromptPolicy::Ask:
            e.request_id = next_request_id();
            m_pending.push_back(Pending{.request_id = e.request_id, .permission = {}, .login = {}, .certificate = error.defer(), .client_certificate = {}});
            break;
        }
        emit(std::move(e));
    };
    ev.on_client_certificate_requested = [this](ClientCertificateRequest& request) {
        switch (m_options.client_certificates) {
        case PromptPolicy::Default: return;
        case PromptPolicy::Deny: request.respond(ClientCertificateAction::Cancel); return;
        case PromptPolicy::Allow:
            request.respond(request.certificates().empty() ? ClientCertificateAction::NoCertificate : ClientCertificateAction::Select, 0);
            return;
        case PromptPolicy::Ask: break;
        }
        GuestSelectClientCertificate e{.id = id(), .request_id = next_request_id(), .host = request.host(), .port = request.port(), .certificates = {}};
        for (const auto& c : request.certificates()) {
            e.certificates.push_back(GuestClientCertificateInfo{.subject = c.subject, .issuer = c.issuer, .display_name = c.display_name});
        }
        m_pending.push_back(Pending{.request_id = e.request_id, .permission = {}, .login = {}, .certificate = {}, .client_certificate = request.defer()});
        emit(std::move(e));
    };

    m_message_hook = m_webview.add_web_message_observer([this](const core::WebView::WebMessage& message) { return handle_web_message(message); });
}

Guest::~Guest() {
    *m_alive = false;
    cancel_pending();
    unbind_native();
    m_webview.remove_web_message_observer(m_message_hook);
    m_webview.events() = core::WebViewEventHandlers{};
    m_find.reset();
}

void Guest::when_ready(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    if (m_webview.is_ready() && m_bound) {
        fn();
        return;
    }
    m_ready_once.push_back(std::move(fn));
}

void Guest::emit(GuestEvent event) {
    m_manager.emit(*this, event);
}

void Guest::on_ready() {
    ICoreWebView2* core = m_webview.core_webview();
    if (core == nullptr) {
        return;
    }
    if (m_bound.Get() != core) {
        unbind_native();
        m_find.reset();
        m_cdp.rebind();
        bind_events();
    }
    if (m_options.zoom_factor != 1.0) {
        m_webview.set_zoom_factor(m_options.zoom_factor);
    }
    if (m_options.console_messages && m_console_sub == 0) {
        set_console_messages(true);
    }
    if (m_placed) {
        const ElementPlacement p = m_placement;
        m_placed = false;
        place(p);
    }
}

void Guest::bind_events() {
    ICoreWebView2* core = m_webview.core_webview();
    ICoreWebView2Controller* controller = m_webview.controller();
    if (core == nullptr || controller == nullptr) {
        return;
    }
    m_bound = core;
    m_bound_controller = controller;
    const std::weak_ptr<bool> alive = m_alive;

    if (const auto core4 = detail::query<ICoreWebView2_4>(m_webview)) {
        BOSON_HR_CHECK(core4->add_DownloadStarting(
            Callback<ICoreWebView2DownloadStartingEventHandler>(
                [this, alive](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                    if (const auto live = alive.lock(); live && *live && args != nullptr) {
                        handle_download(args);
                    }
                    return S_OK;
                })
                .Get(),
            &m_download_token));
    }

    if (const auto core2 = detail::query<ICoreWebView2_2>(m_webview)) {
        BOSON_HR_CHECK(core2->add_DOMContentLoaded(
            Callback<ICoreWebView2DOMContentLoadedEventHandler>(
                [this, alive](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
                    if (const auto live = alive.lock(); live && *live) {
                        emit(GuestDomReady{.id = id()});
                    }
                    return S_OK;
                })
                .Get(),
            &m_dom_token));
    }

    BOSON_HR_CHECK(controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this, alive](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                const auto live = alive.lock();
                if (!live || !*live || args == nullptr) {
                    return S_OK;
                }
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT vk = 0;
                COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                if (FAILED(args->get_KeyEventKind(&kind)) || FAILED(args->get_VirtualKey(&vk)) || FAILED(args->get_PhysicalKeyStatus(&status))) {
                    return S_OK;
                }
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                    return S_OK;
                }
                const menu::KeyModifiers mods = menu::current_modifiers();
                const menu::Accelerator chord{.modifiers = mods, .key = vk};
                const bool reserved = std::ranges::find(m_options.reserved_accelerators, chord) != m_options.reserved_accelerators.end();
                if (!reserved && (!m_options.forward_accelerators || !is_forwarded_accelerator(vk, mods))) {
                    return S_OK;
                }
                if (reserved) {
                    args->put_Handled(TRUE);
                }
                const bool shift = menu::has_modifier(mods, menu::KeyModifiers::Shift);
                emit(GuestAccelerator{
                    .id = id(),
                    .key = dom_key_of(vk, shift),
                    .code = dom_code_of(vk),
                    .key_code = vk,
                    .ctrl = menu::has_modifier(mods, menu::KeyModifiers::Ctrl),
                    .alt = menu::has_modifier(mods, menu::KeyModifiers::Alt),
                    .shift = shift,
                    .meta = menu::has_modifier(mods, menu::KeyModifiers::Win),
                    .repeat = status.WasKeyDown != FALSE,
                    .reserved = reserved,
                });
                return S_OK;
            })
            .Get(),
        &m_accel_token));

    BOSON_HR_CHECK(controller->add_MoveFocusRequested(
        Callback<ICoreWebView2MoveFocusRequestedEventHandler>(
            [this, alive](ICoreWebView2Controller*, ICoreWebView2MoveFocusRequestedEventArgs* args) -> HRESULT {
                const auto live = alive.lock();
                if (!live || !*live || args == nullptr) {
                    return S_OK;
                }
                COREWEBVIEW2_MOVE_FOCUS_REASON reason{};
                if (FAILED(args->get_Reason(&reason)) || reason == COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC) {
                    return S_OK;
                }
                args->put_Handled(TRUE);
                if (core::WebView* owner = m_manager.host().find(m_owner_id); owner != nullptr) {
                    owner->focus();
                }
                emit(GuestFocusOut{.id = id(), .direction = reason == COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT ? "next" : "previous"});
                return S_OK;
            })
            .Get(),
        &m_move_focus_token));

    BOSON_HR_CHECK(controller->add_GotFocus(
        Callback<ICoreWebView2FocusChangedEventHandler>([this, alive](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
            if (const auto live = alive.lock(); live && *live) {
                emit(GuestFocus{.id = id(), .focused = true});
            }
            return S_OK;
        }).Get(),
        &m_got_focus_token));
    BOSON_HR_CHECK(controller->add_LostFocus(
        Callback<ICoreWebView2FocusChangedEventHandler>([this, alive](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
            if (const auto live = alive.lock(); live && *live) {
                emit(GuestFocus{.id = id(), .focused = false});
            }
            return S_OK;
        }).Get(),
        &m_lost_focus_token));
}

void Guest::unbind_native() noexcept {
    for (auto& d : m_downloads) {
        if (d.operation) {
            d.operation->remove_BytesReceivedChanged(d.bytes_token);
            d.operation->remove_StateChanged(d.state_token);
        }
    }
    m_downloads.clear();
    if (m_bound) {
        ComPtr<ICoreWebView2_4> core4;
        if (SUCCEEDED(m_bound.As(&core4))) {
            core4->remove_DownloadStarting(m_download_token);
        }
        ComPtr<ICoreWebView2_2> core2;
        if (SUCCEEDED(m_bound.As(&core2))) {
            core2->remove_DOMContentLoaded(m_dom_token);
        }
        m_bound.Reset();
    }
    if (m_bound_controller) {
        m_bound_controller->remove_AcceleratorKeyPressed(m_accel_token);
        m_bound_controller->remove_MoveFocusRequested(m_move_focus_token);
        m_bound_controller->remove_GotFocus(m_got_focus_token);
        m_bound_controller->remove_LostFocus(m_lost_focus_token);
        m_bound_controller.Reset();
    }
}

bool Guest::handle_web_message(const core::WebView::WebMessage& message) {
    if (message.json.find("\"__bosonGuest\"") == std::string_view::npos) {
        return false;
    }
    wire::GuestWireMessage msg;
    if (glz::read<kLenient>(msg, message.json) || msg.kind != "ipc") {
        return true; // Malformed guest-runtime traffic is dropped, never forwarded.
    }
    emit(GuestIpcMessage{.id = id(), .channel = std::move(msg.channel), .args = std::move(msg.args)});
    return true;
}

void Guest::handle_new_window(core::NewWindowRequest& request) {
    GuestNewWindow e{.id = id(), .url = request.uri(), .disposition = "denied", .new_guest_id = 0, .is_user_initiated = request.is_user_initiated()};
    switch (m_options.popups) {
    case PopupPolicy::Deny:
        request.set_handled();
        break;
    case PopupPolicy::Window:
        e.disposition = "window";
        break;
    case PopupPolicy::Guest: {
        auto deferral = std::make_shared<core::NewWindowDeferral>(request.defer());
        GuestOptions child = m_options;
        child.src.clear();
        Guest* popup = m_manager.create(m_owner_id, std::move(child), "popup", id());
        if (popup == nullptr) {
            deferral->complete_handled();
            break;
        }
        const std::uint64_t popup_id = popup->id();
        GuestManager* manager = &m_manager;
        popup->m_ready_once.push_back([deferral, popup_id, manager]() {
            if (Guest* target = manager->find(popup_id); target != nullptr && deferral->pending()) {
                deferral->complete_with(target->webview());
            }
        });
        e.disposition = "guest";
        e.new_guest_id = popup_id;
        break;
    }
    }
    emit(std::move(e));
}

void Guest::handle_download(ICoreWebView2DownloadStartingEventArgs* args) {
    ComPtr<ICoreWebView2DownloadOperation> op;
    if (!BOSON_HR_CHECK(args->get_DownloadOperation(&op)) || !op) {
        return;
    }
    if (m_options.hide_download_ui) {
        args->put_Handled(TRUE);
    }
    if (!m_options.download_folder.empty()) {
        const std::wstring current = detail::read_wide([&](wchar_t** out_) { return args->get_ResultFilePath(out_); }, "DownloadStarting path");
        const std::filesystem::path target = std::filesystem::path(m_options.download_folder) / std::filesystem::path(current).filename();
        BOSON_HR_CHECK(args->put_ResultFilePath(target.c_str()));
    }
    Download d{.download_id = next_request_id(), .operation = op, .bytes_token = {}, .state_token = {}, .url = {}, .mime_type = {}, .last_report = {}};
    wchar_t* raw = nullptr;
    d.url = detail::read_utf8([&](wchar_t** out_) { return op->get_Uri(out_); }, "DownloadOperation uri");
    raw = nullptr;
    d.mime_type = detail::read_utf8([&](wchar_t** out_) { return op->get_MimeType(out_); }, "DownloadOperation mime");
    const std::uint64_t download_id = d.download_id;
    const std::weak_ptr<bool> alive = m_alive;
    BOSON_HR_CHECK(op->add_BytesReceivedChanged(
        Callback<ICoreWebView2BytesReceivedChangedEventHandler>(
            [this, alive, download_id](ICoreWebView2DownloadOperation*, IUnknown*) -> HRESULT {
                if (const auto live = alive.lock(); live && *live) {
                    report_download(download_id, 1);
                }
                return S_OK;
            })
            .Get(),
        &d.bytes_token));
    BOSON_HR_CHECK(op->add_StateChanged(
        Callback<ICoreWebView2StateChangedEventHandler>([this, alive, download_id](ICoreWebView2DownloadOperation*, IUnknown*) -> HRESULT {
            if (const auto live = alive.lock(); live && *live) {
                report_download(download_id, 2);
            }
            return S_OK;
        }).Get(),
        &d.state_token));
    m_downloads.push_back(std::move(d));
    report_download(download_id, 0);
}

void Guest::report_download(std::uint64_t download_id, int kind) {
    const auto it = std::ranges::find(m_downloads, download_id, &Download::download_id);
    if (it == m_downloads.end()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (kind == 1 && now - it->last_report < std::chrono::milliseconds{100}) {
        return; // progress is coalesced to at most 10 events per second; state changes always report
    }
    it->last_report = now;
    ICoreWebView2DownloadOperation* op = it->operation.Get();
    GuestDownload d{.id = id(), .phase = "updated", .download_id = download_id, .url = it->url, .mime_type = it->mime_type, .path = {}, .total_bytes = -1,
                    .received_bytes = 0, .state = "progressing", .interrupt_reason = {}, .paused = false, .can_resume = false};
    d.path = detail::read_utf8([&](wchar_t** out_) { return op->get_ResultFilePath(out_); }, "DownloadOperation path");
    INT64 total = 0;
    if (SUCCEEDED(op->get_TotalBytesToReceive(&total)) && total > 0) {
        d.total_bytes = total;
    }
    INT64 received = 0;
    if (SUCCEEDED(op->get_BytesReceived(&received))) {
        d.received_bytes = received;
    }
    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    op->get_State(&state);
    COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
    op->get_InterruptReason(&reason);
    BOOL can_resume = FALSE;
    op->get_CanResume(&can_resume);
    d.can_resume = can_resume != FALSE;
    d.interrupt_reason = std::string(interrupt_reason_name(reason));
    bool done = false;
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
        d.state = "completed";
        done = true;
    } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        if (reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED) {
            d.paused = true;
        } else if (reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED) {
            d.state = "cancelled";
            done = true;
        } else {
            d.state = "interrupted";
            done = !d.can_resume;
        }
    }
    if (kind == 0) {
        d.phase = "started";
    } else if (done) {
        d.phase = "done";
        it->operation->remove_BytesReceivedChanged(it->bytes_token);
        it->operation->remove_StateChanged(it->state_token);
        m_downloads.erase(it);
    }
    emit(std::move(d));
}

bool Guest::download_cancel(std::uint64_t download_id) {
    const auto it = std::ranges::find(m_downloads, download_id, &Download::download_id);
    return it != m_downloads.end() && BOSON_HR_CHECK(it->operation->Cancel());
}

bool Guest::download_pause(std::uint64_t download_id) {
    const auto it = std::ranges::find(m_downloads, download_id, &Download::download_id);
    return it != m_downloads.end() && BOSON_HR_CHECK(it->operation->Pause());
}

bool Guest::download_resume(std::uint64_t download_id) {
    const auto it = std::ranges::find(m_downloads, download_id, &Download::download_id);
    return it != m_downloads.end() && BOSON_HR_CHECK(it->operation->Resume());
}

// ---- Layout -------------------------------------------------------------------------------------------------

void Guest::place(const ElementPlacement& placement) {
    const bool first = !m_placed;
    m_placed = true;
    if (!first && placement == m_placement) {
        return;
    }
    const ElementPlacement previous = m_placement;
    m_placement = placement;
    if (!placement.visible) {
        if (m_webview.is_visible()) {
            m_webview.set_visible(false);
        }
        return;
    }
    if (first || !previous.visible || EqualRect(&previous.bounds, &placement.bounds) == FALSE) {
        m_webview.set_bounds(placement.bounds);
    }
    if (first || !previous.visible || previous.clipped != placement.clipped ||
        (placement.clipped && EqualRect(&previous.clip, &placement.clip) == FALSE)) {
        apply_region();
    }
    if (!m_webview.is_visible()) {
        m_webview.set_visible(true);
    }
}

void Guest::apply_region() noexcept {
    HWND container = m_webview.container_hwnd();
    if (container == nullptr) {
        return; // Composition hosting: the visual is not clipped (see docs/WEBVIEW_ELEMENT.md).
    }
    if (!m_placement.clipped) {
        SetWindowRgn(container, nullptr, TRUE);
        return;
    }
    HRGN region = CreateRectRgn(m_placement.clip.left, m_placement.clip.top, m_placement.clip.right, m_placement.clip.bottom);
    if (region != nullptr && SetWindowRgn(container, region, TRUE) == 0) {
        DeleteObject(region); // The system owns the region only when SetWindowRgn succeeds.
    }
}

bool Guest::focus(FocusDirection direction) noexcept {
    ICoreWebView2Controller* controller = m_webview.controller();
    if (controller == nullptr) {
        return false;
    }
    COREWEBVIEW2_MOVE_FOCUS_REASON reason = COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC;
    if (direction == FocusDirection::Next) {
        reason = COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT;
    } else if (direction == FocusDirection::Previous) {
        reason = COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS;
    }
    return BOSON_HR_CHECK(controller->MoveFocus(reason));
}

// ---- Page control -------------------------------------------------------------------------------------------

bool Guest::load_url(std::string_view url) {
    if (url.empty()) {
        return false;
    }
    m_options.src = std::string(url);
    return m_webview.navigate(url);
}

bool Guest::set_zoom_factor(double factor) noexcept {
    if (!(factor > 0.0) || factor > 20.0) {
        return false;
    }
    m_options.zoom_factor = factor;
    return m_webview.set_zoom_factor(factor);
}

bool Guest::set_user_agent(std::string_view user_agent) {
    m_options.user_agent = std::string(user_agent);
    core::WebViewSettings settings;
    settings.user_agent = core::utf8_to_wide(user_agent);
    return m_webview.apply_settings(settings);
}

bool Guest::execute_javascript(std::string_view code, bool user_gesture, std::function<void(bool, std::string)> done) {
    std::string params = R"({"expression":)";
    params += json_string(code);
    params += R"(,"awaitPromise":true,"returnByValue":true,"allowUnsafeEvalBlockedByCSP":true,"userGesture":)";
    params += user_gesture ? "true}" : "false}";
    return m_cdp.call("Runtime.evaluate", params, [done = std::move(done)](bool ok, std::string json) {
        if (!done) {
            return;
        }
        if (!ok) {
            wire::CdpErrorBody err;
            if (glz::read<kLenient>(err, json) || err.message.empty()) {
                err.message = json;
            }
            done(false, std::move(err.message));
            return;
        }
        wire::EvalResult result;
        if (glz::read<kLenient>(result, json)) {
            done(false, "Unreadable Runtime.evaluate result");
            return;
        }
        if (result.exceptionDetails.has_value()) {
            const auto& ex = *result.exceptionDetails;
            std::string message = ex.exception && ex.exception->description ? *ex.exception->description : ex.text;
            done(false, std::move(message));
            return;
        }
        done(true, remote_object_text(result.result));
    });
}

bool Guest::insert_css(std::string_view css, std::function<void(bool, std::string)> done) {
    const std::string key = "boson-css-" + std::to_string(m_next_css_key++);
    std::string js = "(() => { const s = document.createElement('style'); s.setAttribute('data-boson-css', ";
    js += json_string(key);
    js += "); s.textContent = ";
    js += json_string(css);
    js += "; (document.head || document.documentElement).appendChild(s); return true; })()";
    return m_webview.execute_script(js, [key, done = std::move(done)](HRESULT hr, std::string) {
        if (done) {
            done(SUCCEEDED(hr), SUCCEEDED(hr) ? key : std::string{});
        }
    });
}

bool Guest::remove_inserted_css(std::string_view key) {
    std::string js = "(() => { const s = document.querySelector('style[data-boson-css=' + JSON.stringify(";
    js += json_string(key);
    js += ") + ']'); if (s) { s.remove(); return true; } return false; })()";
    return m_webview.execute_script(js, {});
}

bool Guest::send(std::string_view channel, std::string_view args_json) const {
    std::string json = R"({"__bosonGuest":"msg","channel":)";
    json += json_string(channel);
    json += R"(,"args":)";
    json += args_json.empty() ? std::string_view("[]") : args_json;
    json += '}';
    return m_webview.post_web_message_as_json(json);
}

std::uint64_t Guest::find_in_page(std::string_view text, bool forward, bool match_case, bool find_next) {
    if (text.empty() || !m_webview.is_ready()) {
        return 0;
    }
    if (!m_find) {
        m_find = std::make_unique<FindSession>(m_webview);
        m_find->set_state_callback([this](const FindState& s) {
            emit(GuestFoundInPage{
                .id = id(),
                .request_id = m_find_request,
                .matches = s.match_count,
                .active_match_ordinal = std::max(0, s.active_match_index),
                .final_update = true,
            });
        });
    }
    if (!m_find->is_supported()) {
        return 0;
    }
    const std::uint64_t request_id = ++m_find_request;
    if (find_next && m_find->is_active() && m_find_text == text) {
        const bool ok = forward ? m_find->next() : m_find->previous();
        return ok ? request_id : 0;
    }
    m_find_text = std::string(text);
    const FindOptions options{.term = m_find_text, .case_sensitive = match_case, .whole_word = false, .highlight_all = true, .suppress_default_dialog = true};
    const std::weak_ptr<bool> alive = m_alive;
    const bool started = m_find->start(options, [this, alive, request_id](HRESULT hr, FindState s) {
        const auto live = alive.lock();
        if (!live || !*live) {
            return;
        }
        emit(GuestFoundInPage{
            .id = id(),
            .request_id = request_id,
            .matches = SUCCEEDED(hr) ? s.match_count : 0,
            .active_match_ordinal = SUCCEEDED(hr) ? std::max(0, s.active_match_index) : 0,
            .final_update = true,
        });
    });
    return started ? request_id : 0;
}

bool Guest::stop_find_in_page(StopFindAction action) {
    if (!m_find || !m_find->is_supported()) {
        return false;
    }
    if (action == StopFindAction::ActivateSelection) {
        m_webview.execute_script(
            "(() => { const s = window.getSelection(); const n = s && s.anchorNode; const el = n && (n.nodeType === 1 ? n : n.parentElement); "
            "const a = el && el.closest('a,button,[onclick]'); if (a) a.click(); })()",
            {});
    }
    const bool ok = m_find->stop();
    if (action == StopFindAction::ClearSelection) {
        m_webview.execute_script("window.getSelection && window.getSelection().removeAllRanges()", {});
    }
    m_find_text.clear();
    return ok;
}

void Guest::set_console_messages(bool enabled) {
    m_options.console_messages = enabled;
    if (!enabled) {
        m_cdp.unsubscribe(m_console_sub);
        m_cdp.unsubscribe(m_exception_sub);
        m_console_sub = 0;
        m_exception_sub = 0;
        return;
    }
    if (m_console_sub != 0 || m_webview.core_webview() == nullptr) {
        return; // on_ready subscribes once the WebView exists
    }
    m_console_sub = m_cdp.subscribe("Runtime.consoleAPICalled", [this](std::string_view, std::string_view params, std::string_view session) {
        if (!session.empty()) {
            return;
        }
        wire::ConsoleApiCalled call;
        if (glz::read<kLenient>(call, params)) {
            return;
        }
        GuestConsoleMessage e{.id = id(), .level = console_level(call.type), .message = {}, .line = 0, .source_id = {}};
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            if (i != 0) {
                e.message += ' ';
            }
            e.message += console_text(call.args[i]);
        }
        if (call.stackTrace && !call.stackTrace->callFrames.empty()) {
            e.line = call.stackTrace->callFrames.front().lineNumber + 1;
            e.source_id = call.stackTrace->callFrames.front().url;
        }
        emit(std::move(e));
    });
    m_exception_sub = m_cdp.subscribe("Runtime.exceptionThrown", [this](std::string_view, std::string_view params, std::string_view session) {
        if (!session.empty()) {
            return;
        }
        wire::ExceptionThrown ex;
        if (glz::read<kLenient>(ex, params)) {
            return;
        }
        const auto& d = ex.exceptionDetails;
        emit(GuestConsoleMessage{
            .id = id(),
            .level = 3,
            .message = d.exception && d.exception->description ? *d.exception->description : d.text,
            .line = d.lineNumber + 1,
            .source_id = d.url.value_or(std::string{}),
        });
    });
    m_cdp.call("Runtime.enable", "{}", Cdp::ResultFn{});
}

void Guest::cdp_subscribe(std::span<const std::string> events) {
    for (const auto& event : events) {
        if (std::ranges::find(m_js_cdp_subs, event, &std::pair<std::string, Cdp::SubscriptionId>::first) != m_js_cdp_subs.end()) {
            continue;
        }
        const Cdp::SubscriptionId sub = m_cdp.subscribe(event, [this](std::string_view method, std::string_view params, std::string_view session) {
            emit(GuestCdpEvent{.id = id(), .method = std::string(method), .params = glz::raw_json{std::string(params)}, .session_id = std::string(session)});
        });
        if (sub != 0) {
            m_js_cdp_subs.emplace_back(event, sub);
        }
    }
}

void Guest::cdp_unsubscribe(std::span<const std::string> events) {
    for (const auto& event : events) {
        const auto it = std::ranges::find(m_js_cdp_subs, event, &std::pair<std::string, Cdp::SubscriptionId>::first);
        if (it != m_js_cdp_subs.end()) {
            m_cdp.unsubscribe(it->second);
            m_js_cdp_subs.erase(it);
        }
    }
}

// ---- Pending decisions ----------------------------------------------------------------------------------------

bool Guest::respond_permission(std::uint64_t request_id, core::PermissionState state) {
    const auto it = std::ranges::find(m_pending, request_id, &Pending::request_id);
    if (it == m_pending.end() || !it->permission) {
        return false;
    }
    auto fn = std::move(it->permission);
    m_pending.erase(it);
    fn(state);
    return true;
}

bool Guest::respond_login(std::uint64_t request_id, std::optional<Credentials> credentials) {
    const auto it = std::ranges::find(m_pending, request_id, &Pending::request_id);
    if (it == m_pending.end() || !it->login) {
        return false;
    }
    auto fn = std::move(it->login);
    m_pending.erase(it);
    fn(std::move(credentials));
    return true;
}

bool Guest::respond_certificate_error(std::uint64_t request_id, ServerCertificateErrorAction action) {
    const auto it = std::ranges::find(m_pending, request_id, &Pending::request_id);
    if (it == m_pending.end() || !it->certificate) {
        return false;
    }
    auto fn = std::move(it->certificate);
    m_pending.erase(it);
    fn(action);
    return true;
}

bool Guest::respond_client_certificate(std::uint64_t request_id, std::optional<std::size_t> index, bool cancel) {
    const auto it = std::ranges::find(m_pending, request_id, &Pending::request_id);
    if (it == m_pending.end() || !it->client_certificate) {
        return false;
    }
    auto fn = std::move(it->client_certificate);
    m_pending.erase(it);
    if (cancel) {
        fn(ClientCertificateAction::Cancel, 0);
    } else if (index.has_value()) {
        fn(ClientCertificateAction::Select, *index);
    } else {
        fn(ClientCertificateAction::NoCertificate, 0);
    }
    return true;
}

std::size_t Guest::pending_requests() const noexcept {
    return m_pending.size();
}

void Guest::cancel_pending() noexcept {
    auto pending = std::move(m_pending);
    m_pending.clear();
    for (auto& p : pending) {
        try {
            if (p.permission) {
                p.permission(core::PermissionState::Default);
            } else if (p.login) {
                p.login(std::nullopt);
            } else if (p.certificate) {
                p.certificate(ServerCertificateErrorAction::Cancel);
            } else if (p.client_certificate) {
                p.client_certificate(ClientCertificateAction::Cancel, 0);
            }
        } catch (...) {
            // A completion must never throw out of teardown.
        }
    }
}

// ---- GuestManager -------------------------------------------------------------------------------------------

GuestManager::GuestManager(core::WebViewHost& host, GuestManagerOptions options) : m_host(host), m_options(std::move(options)) {}

GuestManager::~GuestManager() {
    m_listeners.clear();
    destroy_all();
}

std::optional<std::string> GuestManager::resolve_preload(std::string_view url) const {
    if (url.empty() || !m_options.resolve_preload) {
        return std::nullopt;
    }
    return m_options.resolve_preload(url);
}

Guest* GuestManager::create(std::uint64_t owner_id, GuestOptions options, std::string_view reason, std::uint64_t opener_id) {
    const auto partition = parse_partition(options.partition);
    if (!partition.has_value()) {
        return nullptr;
    }
    if (m_options.max_guests_per_owner != 0) {
        const auto owned = std::ranges::count_if(m_guests, [owner_id](const auto& g) { return g->owner_id() == owner_id; });
        if (static_cast<std::size_t>(owned) >= m_options.max_guests_per_owner) {
            return nullptr;
        }
    }
    core::WebViewConfig config;
    config.initial_url = options.src;
    config.serve_boson_assets = false;
    config.start_visible = false;
    config.profile_name = partition->profile_name;
    config.in_private = partition->in_private;
    config.script_locale = options.script_locale;
    config.settings.devtools_enabled = options.devtools;
    config.settings.default_context_menus_enabled = options.context_menus;
    config.settings.web_messages_enabled = true;
    if (!options.user_agent.empty()) {
        config.settings.user_agent = core::utf8_to_wide(options.user_agent);
    }
    config.scripts_on_document_created.push_back(Guest::guest_runtime_script());
    if (!options.preload_script.empty()) {
        // Function scope keeps the preload's declarations out of the page's globals; top frame only.
        config.scripts_on_document_created.push_back(
            L"(function () { if (window.top !== window) return;\n" + core::utf8_to_wide(options.preload_script) + L"\n})();");
    }

    auto slot = std::make_shared<Guest*>(nullptr);
    core::WebView& webview = m_host.create(
        std::move(config),
        core::WebViewCallbacks{
            .asset_lookup = {},
            .on_message = {},
            .on_ready =
                [slot]() {
                    Guest* guest = *slot;
                    if (guest == nullptr) {
                        return;
                    }
                    guest->on_ready();
                    auto once = std::move(guest->m_ready_once);
                    guest->m_ready_once.clear();
                    for (auto& fn : once) {
                        fn();
                    }
                },
        });
    webview.set_visible(false);
    auto guest = std::make_unique<Guest>(*this, webview, owner_id, std::move(options), opener_id);
    Guest* raw = guest.get();
    *slot = raw;
    raw->m_destroy_slot = slot;
    m_guests.push_back(std::move(guest));
    emit(*raw, GuestCreated{.id = raw->id(), .opener_id = opener_id, .url = raw->options().src, .reason = std::string(reason)});
    return raw;
}

bool GuestManager::destroy(std::uint64_t guest_id) {
    const auto it = std::ranges::find_if(m_guests, [guest_id](const auto& g) { return g->id() == guest_id; });
    if (it == m_guests.end()) {
        return false;
    }
    std::unique_ptr<Guest> guest = std::move(*it);
    m_guests.erase(it);
    emit(*guest, GuestDestroyed{.id = guest_id});
    if (guest->m_destroy_slot) {
        *guest->m_destroy_slot = nullptr;
    }
    core::WebView& webview = guest->webview();
    guest.reset();
    m_host.destroy(webview);
    return true;
}

std::size_t GuestManager::destroy_owned_by(std::uint64_t owner_id) {
    std::vector<std::uint64_t> ids;
    for (const auto& g : m_guests) {
        if (g->owner_id() == owner_id) {
            ids.push_back(g->id());
        }
    }
    for (const std::uint64_t id : ids) {
        destroy(id);
    }
    return ids.size();
}

void GuestManager::destroy_all() {
    while (!m_guests.empty()) {
        destroy(m_guests.back()->id());
    }
}

Guest* GuestManager::find(std::uint64_t guest_id) const noexcept {
    for (const auto& g : m_guests) {
        if (g->id() == guest_id) {
            return g.get();
        }
    }
    return nullptr;
}

std::vector<Guest*> GuestManager::guests() const {
    std::vector<Guest*> out;
    out.reserve(m_guests.size());
    for (const auto& g : m_guests) {
        out.push_back(g.get());
    }
    return out;
}

std::size_t GuestManager::apply_layout(std::uint64_t owner_id, std::span<const GuestLayoutItem> items, double device_pixel_ratio) {
    POINT origin{};
    if (const core::WebView* owner = m_host.find(owner_id); owner != nullptr) {
        origin = POINT{owner->bounds().left, owner->bounds().top};
    }
    std::size_t changed = 0;
    bool restack_needed = false;
    for (const GuestLayoutItem& item : items) {
        Guest* guest = find(item.id);
        if (guest == nullptr || guest->owner_id() != owner_id) {
            continue;
        }
        const ElementPlacement placement = compute_element_placement(item.rect, item.clip, device_pixel_ratio, origin, item.visible);
        if (!guest->m_placed || !(placement == guest->placement())) {
            guest->place(placement);
            ++changed;
        }
        if (guest->z() != item.z || !guest->m_stacked) {
            guest->set_z(item.z);
            guest->m_stacked = true;
            restack_needed = true;
        }
    }
    if (restack_needed) {
        restack(owner_id);
    }
    return changed;
}

void GuestManager::restack(std::uint64_t owner_id) {
    std::vector<Guest*> owned;
    for (const auto& g : m_guests) {
        if (g->owner_id() == owner_id) {
            owned.push_back(g.get());
        }
    }
    std::ranges::stable_sort(owned, {}, &Guest::z);
    for (Guest* g : owned) {
        g->webview().bring_to_front();
    }
}

GuestManager::ListenerId GuestManager::add_listener(Listener listener) {
    if (!listener) {
        return 0;
    }
    const ListenerId id = m_next_listener++;
    m_listeners.push_back(ListenerEntry{.id = id, .fn = std::make_shared<Listener>(std::move(listener))});
    return id;
}

bool GuestManager::remove_listener(ListenerId id) noexcept {
    return std::erase_if(m_listeners, [id](const ListenerEntry& e) { return e.id == id; }) != 0;
}

void GuestManager::emit(Guest& guest, const GuestEvent& event) {
    if (m_listeners.empty()) {
        return;
    }
    // Copy: a listener may add or remove listeners (or destroy guests) while running.
    std::vector<std::shared_ptr<Listener>> listeners;
    listeners.reserve(m_listeners.size());
    for (const auto& e : m_listeners) {
        listeners.push_back(e.fn);
    }
    for (const auto& fn : listeners) {
        try {
            (*fn)(guest, event);
        } catch (...) {
            // Listener exceptions never cross the WebView2 event boundary.
        }
    }
}

} // namespace boson::webview
