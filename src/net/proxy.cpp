#include <boson/net/proxy.hpp>

#include <boson/core/result.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace boson::net {

namespace {

struct GlobalFree {
    void operator()(wchar_t* p) const noexcept {
        if (p != nullptr) {
            ::GlobalFree(p);
        }
    }
};
using GlobalString = std::unique_ptr<wchar_t, GlobalFree>;

std::string take(wchar_t* raw) {
    const GlobalString owned(raw);
    return raw == nullptr ? std::string{} : core::wide_to_utf8(raw);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// '*' matches any run of characters; case-insensitive (inputs already lower-cased).
bool wildcard_match(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == text[t] || pattern[p] == '?')) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

} // namespace

std::optional<SystemProxyConfig> read_system_proxy_config() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
        return std::nullopt;
    }
    SystemProxyConfig out;
    out.auto_detect = ie.fAutoDetect != FALSE;
    out.auto_config_url = take(ie.lpszAutoConfigUrl);
    out.proxy = take(ie.lpszProxy);
    out.bypass = take(ie.lpszProxyBypass);
    return out;
}

std::vector<ProxyEntry> parse_proxy_list(std::string_view list) {
    std::vector<ProxyEntry> out;
    std::size_t pos = 0;
    while (pos < list.size()) {
        const std::size_t end = list.find_first_of("; \t\r\n", pos);
        std::string_view item = list.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? list.size() : end + 1;
        if (item.empty()) {
            continue;
        }
        ProxyEntry entry;
        if (const auto eq = item.find('='); eq != std::string_view::npos) {
            entry.scheme = lower(item.substr(0, eq));
            item.remove_prefix(eq + 1);
            if (entry.scheme == "socks") {
                entry.proxy_scheme = "socks";
            }
        }
        if (const auto sep = item.find("://"); sep != std::string_view::npos) {
            entry.proxy_scheme = lower(item.substr(0, sep));
            item.remove_prefix(sep + 3);
        }
        while (!item.empty() && item.back() == '/') {
            item.remove_suffix(1);
        }
        std::string_view host = item;
        if (!item.empty() && item.front() == '[') {
            const auto close = item.find(']');
            if (close == std::string_view::npos) {
                continue;
            }
            host = item.substr(0, close + 1);
            item.remove_prefix(close + 1);
            if (!item.empty() && item.front() == ':') {
                item.remove_prefix(1);
            } else {
                item = {};
            }
        } else if (const auto colon = item.rfind(':'); colon != std::string_view::npos) {
            host = item.substr(0, colon);
            item.remove_prefix(colon + 1);
        } else {
            item = {};
        }
        if (host.empty()) {
            continue;
        }
        bool valid_port = true;
        unsigned port = 0;
        for (const char c : item) {
            if (c < '0' || c > '9') {
                valid_port = false;
                break;
            }
            port = port * 10 + static_cast<unsigned>(c - '0');
            if (port > 65535) {
                valid_port = false;
                break;
            }
        }
        if (!valid_port) {
            continue;
        }
        entry.host = lower(host);
        entry.port = static_cast<std::uint16_t>(port);
        out.push_back(std::move(entry));
    }
    return out;
}

std::optional<ProxyEntry> proxy_for_scheme(const std::vector<ProxyEntry>& entries, std::string_view scheme) {
    const std::string s = lower(scheme);
    for (const auto& e : entries) {
        if (e.scheme == s) {
            return e;
        }
    }
    for (const auto& e : entries) {
        if (e.scheme.empty()) {
            return e;
        }
    }
    return std::nullopt;
}

bool proxy_bypassed(std::string_view host_in, std::string_view bypass_list) {
    const std::string host = lower(host_in);
    std::size_t pos = 0;
    while (pos < bypass_list.size()) {
        const std::size_t end = bypass_list.find_first_of("; \t\r\n", pos);
        const std::string_view item = bypass_list.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? bypass_list.size() : end + 1;
        if (item.empty()) {
            continue;
        }
        std::string pattern = lower(item);
        if (pattern == "<local>") {
            if (host.find('.') == std::string::npos && host.find(':') == std::string::npos) {
                return true;
            }
            continue;
        }
        if (const auto sep = pattern.find("://"); sep != std::string::npos) {
            pattern.erase(0, sep + 3);
        }
        // "host:port" patterns: the port is not part of host matching here.
        if (const auto colon = pattern.rfind(':'); colon != std::string::npos && pattern.find(']') == std::string::npos &&
                                                   pattern.find(':') == colon) {
            pattern.erase(colon);
        }
        if (wildcard_match(pattern, host)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> resolve_system_proxy(std::string_view url) {
    const auto config = read_system_proxy_config();
    if (!config) {
        return std::nullopt;
    }
    const std::wstring wide_url = core::utf8_to_wide(url);
    if (config->auto_detect || !config->auto_config_url.empty()) {
        const HINTERNET session = WinHttpOpen(L"Boson proxy resolver", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session != nullptr) {
            WINHTTP_AUTOPROXY_OPTIONS auto_options{};
            const std::wstring pac = core::utf8_to_wide(config->auto_config_url);
            if (!pac.empty()) {
                auto_options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
                auto_options.lpszAutoConfigUrl = pac.c_str();
            } else {
                auto_options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
                auto_options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
            }
            auto_options.fAutoLogonIfChallenged = TRUE;
            WINHTTP_PROXY_INFO info{};
            const BOOL ok = WinHttpGetProxyForUrl(session, wide_url.c_str(), &auto_options, &info);
            WinHttpCloseHandle(session);
            if (ok) {
                const std::string proxy = take(info.lpszProxy);
                (void)take(info.lpszProxyBypass);
                if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && !proxy.empty()) {
                    return proxy;
                }
                return std::nullopt;
            }
        }
    }
    if (config->proxy.empty()) {
        return std::nullopt;
    }
    // Static configuration: honour the bypass list for the URL's host.
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts)) {
        return std::nullopt;
    }
    const std::string host = core::wide_to_utf8(std::wstring_view(parts.lpszHostName, parts.dwHostNameLength));
    const std::string scheme = core::wide_to_utf8(std::wstring_view(parts.lpszScheme, parts.dwSchemeLength));
    if (proxy_bypassed(host, config->bypass)) {
        return std::nullopt;
    }
    const auto entry = proxy_for_scheme(parse_proxy_list(config->proxy), scheme);
    if (!entry) {
        return std::nullopt;
    }
    return entry->port != 0 ? entry->host + ":" + std::to_string(entry->port) : entry->host;
}

} // namespace boson::net
