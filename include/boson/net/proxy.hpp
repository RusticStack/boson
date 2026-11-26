#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::net {

/// @brief How HttpClient reaches the network.
enum class ProxyMode : std::uint8_t {
    System,   ///< WinHTTP automatic proxy: the user's settings (IE / Settings app), PAC script and WPAD
    Direct,   ///< no proxy
    Explicit, ///< ProxySettings::server / bypass
};

struct ProxySettings {
    ProxyMode mode = ProxyMode::System;
    /// Explicit mode: "host:port", "http=h:p;https=h2:p2" (WinHTTP / IE syntax).
    std::string server;
    /// Explicit mode: "localhost;*.corp;<local>" - hosts that bypass the proxy.
    std::string bypass;
};

/// @brief The current user's proxy configuration (WinHttpGetIEProxyConfigForCurrentUser).
struct SystemProxyConfig {
    bool auto_detect = false;       ///< WPAD
    std::string auto_config_url;    ///< PAC script URL
    std::string proxy;              ///< static proxy list (IE syntax)
    std::string bypass;             ///< bypass list (';' or whitespace separated)
};

[[nodiscard]] std::optional<SystemProxyConfig> read_system_proxy_config();

/// @brief One entry of a proxy list: "http=proxy:8080" -> {scheme "http", host "proxy", port 8080}.
/// scheme is empty for entries without "scheme=" (they apply to every scheme).
struct ProxyEntry {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0; ///< 0 when the entry has no port (WinHTTP uses 80)
    std::string proxy_scheme; ///< "http" / "socks" / "" when the entry names one ("socks=host:1080" or "http://h:p")
};

/// @brief Parses an IE / WinHTTP proxy list ("h:p", "http=h:p;https=h2:p2", "http://h:p", whitespace or ';'
/// separated). Malformed entries are skipped.
[[nodiscard]] std::vector<ProxyEntry> parse_proxy_list(std::string_view list);
/// @brief Entry used for a URL scheme ("http" / "https"): a scheme-specific entry first, then a generic one.
[[nodiscard]] std::optional<ProxyEntry> proxy_for_scheme(const std::vector<ProxyEntry>& entries, std::string_view scheme);
/// @brief Bypass test with IE semantics: '*' wildcards, case-insensitive, optional ":port", and "<local>"
/// (host names without a dot).
[[nodiscard]] bool proxy_bypassed(std::string_view host, std::string_view bypass_list);

/// @brief The proxy WinHTTP would use for url with the current user's settings (PAC / WPAD evaluated with
/// WinHttpGetProxyForUrl). std::nullopt = direct. Blocking (a PAC download may take seconds).
[[nodiscard]] std::optional<std::string> resolve_system_proxy(std::string_view url);

} // namespace boson::net
