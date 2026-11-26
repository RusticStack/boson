#pragma once

#include <boson/core/environment.hpp>

#include <optional>
#include <string>
#include <vector>

namespace boson::webview {

enum class ProxyMode {
    /// No proxy switches: the runtime follows the system (WinINet) proxy settings.
    System,
    /// --no-proxy-server: always connect directly.
    Direct,
    /// --proxy-auto-detect: WPAD discovery.
    AutoDetect,
    /// --proxy-server=<server> [--proxy-bypass-list=<...>]
    Fixed,
    /// --proxy-pac-url=<url>
    PacScript,
};

/// @brief Chromium proxy switches. WebView2 has no runtime or per-profile proxy API (SDK 1.0.4370.26), so a
/// proxy is fixed per Environment (browser process); changing it requires Environment::recreate().
struct ProxyConfig {
    ProxyMode mode = ProxyMode::System;
    /// Fixed: "host:port", "scheme://host:port", or a per-scheme list such as
    /// "http=proxy:8080;https=proxy:8443;socks=socks5://proxy:1080".
    std::wstring server;
    /// Fixed: hosts that bypass the proxy, e.g. {L"*.corp.example", L"<local>", L"10.0.0.0/8"}.
    std::vector<std::wstring> bypass_list;
    /// PacScript: http(s):// or data: URL of the PAC script.
    std::wstring pac_url;
};

/// @brief Returns the first problem in config (empty fields for the mode, whitespace or quotes in values).
[[nodiscard]] std::optional<std::string> validate_proxy_config(const ProxyConfig& config);

/// @brief The switches for config, in order. Empty for ProxyMode::System or an invalid config.
[[nodiscard]] std::vector<std::wstring> proxy_switches(const ProxyConfig& config);

/// @brief Removes every proxy switch already in env.additional_browser_arguments and appends config's.
/// Returns false (env untouched) when config is invalid.
bool apply_proxy(core::EnvironmentConfig& env, const ProxyConfig& config);

} // namespace boson::webview
