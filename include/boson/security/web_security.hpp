#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boson/core/webview.hpp>
#include <boson/security/origin.hpp>

namespace boson::security {

inline constexpr std::string_view kDefaultPermissionsPolicy =
    "camera=(), microphone=(), geolocation=(), payment=(), usb=(), serial=(), hid=(), bluetooth=(), "
    "midi=(), display-capture=(), publickey-credentials-get=()";

/// @brief Security headers for boson://app responses.
struct SecurityHeaders {
    /// false = no Content-Security-Policy header at all.
    bool csp_enabled = true;
    /// Full CSP; empty = default_content_security_policy(dev_url) is used.
    std::string content_security_policy;
    /// Extra sources appended to script-src / connect-src / etc. of the default policy, e.g. "https://api.example.com".
    std::vector<std::string> extra_connect_src;
    std::vector<std::string> extra_script_src;
    std::vector<std::string> extra_img_src;
    /// true = Content-Security-Policy-Report-Only instead of enforcing.
    bool report_only = false;
    /// X-Content-Type-Options: nosniff
    bool nosniff = true;
    /// Referrer-Policy; empty = omitted.
    std::string referrer_policy = "no-referrer";
    /// Permissions-Policy; empty = omitted.
    std::string permissions_policy = std::string(kDefaultPermissionsPolicy);
    /// Cross-Origin-Opener-Policy; empty = omitted.
    std::string cross_origin_opener_policy = "same-origin";
    /// Any further raw "Name: value" header lines.
    std::vector<std::string> extra_headers;
};

/// @brief Strict default CSP for an embedded app. With dev_url set, the dev server origin and its
/// WebSocket (HMR) are added to script-src / connect-src so assets served from boson://app keep working
/// for a page loaded from the dev server.
[[nodiscard]] std::string default_content_security_policy(const std::optional<std::string>& dev_url = std::nullopt);

/// @brief The CRLF-separated header block for options (no leading/trailing CRLF).
[[nodiscard]] std::string build_security_headers(const SecurityHeaders& options, const std::optional<std::string>& dev_url);

/// @brief Writes the header block into config.asset_response_headers (call before WebViewHost::create /
/// WebView::initialize). Header names/values with CR or LF are rejected (header injection); returns false then.
bool apply_security_headers(core::WebViewConfig& config, const SecurityHeaders& options = {});

// ---- Navigation allow-list ------------------------------------------------------------------------------

enum class NavigationAction {
    Allow,
    Block,
    OpenExternal, ///< cancel in the WebView, open in the user's default browser
};

struct NavigationPolicy {
    /// Origin patterns the WebView may navigate to. Empty = default_app_origins(config.dev_url).
    std::vector<std::string> allowed_origins;
    /// Origins sent to the system browser instead of being blocked (e.g. "https://*" ... ).
    std::vector<std::string> external_origins;
    /// What happens to a URI that matches neither list.
    NavigationAction otherwise = NavigationAction::Block;
    /// Schemes ShellExecute may receive; anything else is only blocked.
    std::vector<std::string> external_schemes = {"https", "http", "mailto"};
    bool allow_about_blank = true;
    /// Replaces ShellExecuteExW (tests, or custom handling). Called on the UI thread.
    std::function<void(std::string_view uri)> open_external;
    /// Observes every decision that is not Allow (logging / telemetry). UI thread.
    std::function<void(std::string_view uri, NavigationAction action)> on_rejected;
};

/// @brief Pure decision function (no side effects): what policy does with uri.
class CompiledNavigationPolicy {
public:
    CompiledNavigationPolicy() = default;
    CompiledNavigationPolicy(const NavigationPolicy& policy, const std::optional<std::string>& dev_url);

    [[nodiscard]] NavigationAction decide(std::string_view uri) const noexcept;
    [[nodiscard]] bool is_external_scheme_allowed(std::string_view uri) const noexcept;

private:
    std::vector<OriginPattern> m_allowed;
    std::vector<OriginPattern> m_external;
    std::vector<std::string> m_external_schemes;
    NavigationAction m_otherwise = NavigationAction::Block;
    bool m_allow_about_blank = true;
};

/// @brief Opens uri with the default handler (ShellExecuteExW, SEE_MASK_ASYNCOK so the UI thread does not
/// wait). Only http/https/mailto URIs are accepted; false otherwise.
bool open_external_uri(std::string_view uri) noexcept;

/// @brief Installs the policy on webview.events().on_navigation_starting. An existing handler keeps running
/// for allowed navigations (it is chained, not replaced).
void install_navigation_policy(core::WebView& webview, NavigationPolicy policy);

// ---- New-window (window.open / target=_blank) policy -----------------------------------------------------

enum class NewWindowAction {
    AllowDefault, ///< let the next handler (or WebView2's default popup) deal with it
    Block,
    OpenExternal,
    NavigateSelf, ///< load it in the requesting WebView instead of a popup
};

struct NewWindowPolicy {
    /// Origins for which the request is passed on (AllowDefault). Empty = nothing.
    std::vector<std::string> allowed_origins;
    /// Origins opened in the system browser.
    std::vector<std::string> external_origins = {"https:*", "http:*"};
    NewWindowAction otherwise = NewWindowAction::Block;
    /// Action for allowed_origins (AllowDefault or NavigateSelf).
    NewWindowAction allowed_action = NewWindowAction::AllowDefault;
    std::function<void(std::string_view uri)> open_external;
    std::function<void(std::string_view uri, NewWindowAction action)> on_rejected;
};

[[nodiscard]] NewWindowAction decide_new_window(
    std::span<const OriginPattern> allowed,
    std::span<const OriginPattern> external,
    NewWindowAction allowed_action,
    NewWindowAction otherwise,
    std::string_view uri) noexcept;

/// @brief Installs the policy on webview.events().on_new_window_requested, chaining an existing handler
/// for AllowDefault decisions.
void install_new_window_policy(core::WebView& webview, NewWindowPolicy policy);

} // namespace boson::security
