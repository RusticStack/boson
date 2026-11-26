#include <boson/webview/cookies.hpp>

#include "detail.hpp"

#include <cmath>
#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

COREWEBVIEW2_COOKIE_SAME_SITE_KIND to_native(CookieSameSite v) noexcept {
    switch (v) {
    case CookieSameSite::None: return COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
    case CookieSameSite::Strict: return COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT;
    case CookieSameSite::Lax: break;
    }
    return COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX;
}

CookieSameSite from_native(COREWEBVIEW2_COOKIE_SAME_SITE_KIND v) noexcept {
    switch (v) {
    case COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE: return CookieSameSite::None;
    case COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT: return CookieSameSite::Strict;
    case COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX: break;
    }
    return CookieSameSite::Lax;
}

Cookie read_cookie(ICoreWebView2Cookie* native) {
    Cookie out;
    wchar_t* raw = nullptr;
    HRESULT hr = native->get_Name(&raw);
    out.name = detail::take_utf8(hr, raw, "Cookie::get_Name");
    raw = nullptr;
    hr = native->get_Value(&raw);
    out.value = detail::take_utf8(hr, raw, "Cookie::get_Value");
    raw = nullptr;
    hr = native->get_Domain(&raw);
    out.domain = detail::take_utf8(hr, raw, "Cookie::get_Domain");
    raw = nullptr;
    hr = native->get_Path(&raw);
    out.path = detail::take_utf8(hr, raw, "Cookie::get_Path");
    BOOL flag = FALSE;
    if (SUCCEEDED(native->get_IsSession(&flag))) {
        out.is_session = flag != FALSE;
    }
    double expires = -1.0;
    if (!out.is_session && SUCCEEDED(native->get_Expires(&expires)) && expires >= 0.0) {
        out.expires = expires;
    }
    flag = FALSE;
    if (SUCCEEDED(native->get_IsHttpOnly(&flag))) {
        out.http_only = flag != FALSE;
    }
    flag = FALSE;
    if (SUCCEEDED(native->get_IsSecure(&flag))) {
        out.secure = flag != FALSE;
    }
    COREWEBVIEW2_COOKIE_SAME_SITE_KIND same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX;
    if (SUCCEEDED(native->get_SameSite(&same_site))) {
        out.same_site = from_native(same_site);
    }
    return out;
}

bool has_forbidden_char(std::string_view text, std::string_view forbidden) noexcept {
    for (const char ch : text) {
        const auto uch = static_cast<unsigned char>(ch);
        if (uch < 0x20 || uch == 0x7F || forbidden.find(ch) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view to_string(CookieSameSite same_site) noexcept {
    switch (same_site) {
    case CookieSameSite::None: return "none";
    case CookieSameSite::Strict: return "strict";
    case CookieSameSite::Lax: break;
    }
    return "lax";
}

std::optional<CookieSameSite> parse_cookie_same_site(std::string_view name) noexcept {
    if (name == "none" || name == "None") return CookieSameSite::None;
    if (name == "lax" || name == "Lax") return CookieSameSite::Lax;
    if (name == "strict" || name == "Strict") return CookieSameSite::Strict;
    return std::nullopt;
}

std::optional<std::string> validate_cookie(const Cookie& cookie) {
    if (cookie.name.empty()) {
        return "cookie name must not be empty";
    }
    if (has_forbidden_char(cookie.name, " \t\"(),/:;<=>?@[\\]{}")) {
        return "cookie name contains a forbidden character";
    }
    if (has_forbidden_char(cookie.value, ";")) {
        return "cookie value contains a control character or ';'";
    }
    if (cookie.domain.empty()) {
        return "cookie domain must not be empty";
    }
    if (has_forbidden_char(cookie.domain, " /;:")) {
        return "cookie domain must be a bare host name such as \"example.com\" or \".example.com\"";
    }
    if (cookie.path.empty() || cookie.path.front() != '/') {
        return "cookie path must start with '/'";
    }
    if (cookie.same_site == CookieSameSite::None && !cookie.secure) {
        return "SameSite=None cookies must be secure";
    }
    if (cookie.expires.has_value() && (!std::isfinite(*cookie.expires) || *cookie.expires < 0.0)) {
        return "cookie expires must be a non-negative UNIX time in seconds";
    }
    return std::nullopt;
}

ComPtr<ICoreWebView2CookieManager> cookie_manager(const core::WebView& webview) noexcept {
    ComPtr<ICoreWebView2CookieManager> manager;
    if (const auto webview2 = detail::require<ICoreWebView2_2>(webview, "ICoreWebView2_2 (CookieManager)")) {
        if (!BOSON_HR_CHECK(webview2->get_CookieManager(&manager))) {
            manager.Reset();
        }
    }
    return manager;
}

bool get_cookies(core::WebView& webview, std::string_view uri, std::function<void(HRESULT, std::vector<Cookie>)> callback) {
    const auto manager = cookie_manager(webview);
    if (!manager) {
        return false;
    }
    const std::wstring wide = core::utf8_to_wide(uri);
    return BOSON_HR_CHECK(manager->GetCookies(
        wide.c_str(),
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, ICoreWebView2CookieList* list) -> HRESULT {
                std::vector<Cookie> cookies;
                if (SUCCEEDED(hr) && list != nullptr) {
                    UINT32 count = 0;
                    BOSON_HR_CHECK(list->get_Count(&count));
                    cookies.reserve(count);
                    for (UINT32 i = 0; i < count; ++i) {
                        ComPtr<ICoreWebView2Cookie> cookie;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &cookie)) && cookie) {
                            cookies.push_back(read_cookie(cookie.Get()));
                        }
                    }
                }
                if (callback) {
                    callback(hr, std::move(cookies));
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<std::vector<Cookie>> get_cookies(core::WebView& webview, std::string uri) {
    return exec::CallbackSender<std::vector<Cookie>>(
        [&webview, uri = std::move(uri)](exec::Completion<std::vector<Cookie>> done) {
            if (!get_cookies(webview, uri, [done](HRESULT hr, std::vector<Cookie> cookies) { done(hr, std::move(cookies)); })) {
                done(E_FAIL, {});
            }
        },
        "ICoreWebView2CookieManager::GetCookies");
}

bool set_cookie(core::WebView& webview, const Cookie& cookie) {
    if (const auto problem = validate_cookie(cookie)) {
        core::hr_check(E_INVALIDARG, *problem);
        return false;
    }
    const auto manager = cookie_manager(webview);
    if (!manager) {
        return false;
    }
    const std::wstring name = core::utf8_to_wide(cookie.name);
    const std::wstring value = core::utf8_to_wide(cookie.value);
    const std::wstring domain = core::utf8_to_wide(cookie.domain);
    const std::wstring path = core::utf8_to_wide(cookie.path);
    ComPtr<ICoreWebView2Cookie> native;
    if (!BOSON_HR_CHECK(manager->CreateCookie(name.c_str(), value.c_str(), domain.c_str(), path.c_str(), &native)) || !native) {
        return false;
    }
    if (cookie.expires.has_value() && !BOSON_HR_CHECK(native->put_Expires(*cookie.expires))) {
        return false;
    }
    if (!BOSON_HR_CHECK(native->put_IsHttpOnly(cookie.http_only ? TRUE : FALSE)) ||
        !BOSON_HR_CHECK(native->put_IsSecure(cookie.secure ? TRUE : FALSE)) ||
        !BOSON_HR_CHECK(native->put_SameSite(to_native(cookie.same_site)))) {
        return false;
    }
    return BOSON_HR_CHECK(manager->AddOrUpdateCookie(native.Get()));
}

bool delete_cookies(core::WebView& webview, std::string_view name, std::string_view uri) {
    const auto manager = cookie_manager(webview);
    if (!manager || name.empty()) {
        return false;
    }
    const std::wstring wname = core::utf8_to_wide(name);
    const std::wstring wuri = core::utf8_to_wide(uri);
    return BOSON_HR_CHECK(manager->DeleteCookies(wname.c_str(), wuri.c_str()));
}

bool delete_cookies(core::WebView& webview, std::string_view name, std::string_view domain, std::string_view path) {
    const auto manager = cookie_manager(webview);
    if (!manager || name.empty()) {
        return false;
    }
    const std::wstring wname = core::utf8_to_wide(name);
    const std::wstring wdomain = core::utf8_to_wide(domain);
    const std::wstring wpath = core::utf8_to_wide(path);
    return BOSON_HR_CHECK(manager->DeleteCookiesWithDomainAndPath(wname.c_str(), wdomain.c_str(), wpath.c_str()));
}

bool delete_all_cookies(core::WebView& webview) {
    const auto manager = cookie_manager(webview);
    return manager && BOSON_HR_CHECK(manager->DeleteAllCookies());
}

} // namespace boson::webview
