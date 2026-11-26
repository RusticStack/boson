#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/callback_sender.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview {

enum class CookieSameSite {
    None,
    Lax,
    Strict,
};

[[nodiscard]] std::string_view to_string(CookieSameSite same_site) noexcept;
[[nodiscard]] std::optional<CookieSameSite> parse_cookie_same_site(std::string_view name) noexcept;

/// @brief One cookie (ICoreWebView2Cookie). Strings are UTF-8.
struct Cookie {
    std::string name;
    std::string value;
    /// Host-only when it has no leading dot ("example.com"); ".example.com" also matches subdomains.
    std::string domain;
    std::string path = "/";
    /// Seconds since the UNIX epoch; nullopt = session cookie.
    std::optional<double> expires;
    bool http_only = false;
    bool secure = false;
    CookieSameSite same_site = CookieSameSite::Lax;
    bool is_session = true; ///< Read-only: reported by the runtime, ignored by set_cookie.
};

/// @brief Checks the attributes the runtime would reject (or silently drop) before anything reaches COM.
[[nodiscard]] std::optional<std::string> validate_cookie(const Cookie& cookie);

/// @brief The cookie manager of the WebView's profile (ICoreWebView2_2::get_CookieManager).
[[nodiscard]] Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> cookie_manager(const core::WebView& webview) noexcept;

/// @brief Cookies that would be sent to uri; an empty uri returns every cookie in the profile. UI thread.
bool get_cookies(core::WebView& webview, std::string_view uri, std::function<void(HRESULT, std::vector<Cookie>)> callback);
[[nodiscard]] exec::CallbackSender<std::vector<Cookie>> get_cookies(core::WebView& webview, std::string uri);

/// @brief AddOrUpdateCookie. Synchronous on the UI thread; false when invalid or rejected.
bool set_cookie(core::WebView& webview, const Cookie& cookie);
/// @brief Deletes cookies named name that would be sent to uri (DeleteCookies).
bool delete_cookies(core::WebView& webview, std::string_view name, std::string_view uri);
/// @brief Deletes cookies with this exact name, domain and path (DeleteCookiesWithDomainAndPath).
bool delete_cookies(core::WebView& webview, std::string_view name, std::string_view domain, std::string_view path);
bool delete_all_cookies(core::WebView& webview);

} // namespace boson::webview
