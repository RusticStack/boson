#include <boson/webview/proxy.hpp>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>
#include <utility>

namespace boson::webview {

namespace {

constexpr std::array<std::wstring_view, 5> kProxySwitches{
    L"--proxy-server",
    L"--proxy-bypass-list",
    L"--proxy-pac-url",
    L"--proxy-auto-detect",
    L"--no-proxy-server",
};

bool has_space_or_quote(std::wstring_view text) noexcept {
    return std::ranges::any_of(text, [](wchar_t ch) { return ch == L'"' || std::iswspace(ch) != 0; });
}

bool is_proxy_switch(std::wstring_view token) noexcept {
    return std::ranges::any_of(kProxySwitches, [token](std::wstring_view sw) {
        return token == sw || (token.starts_with(sw) && token.size() > sw.size() && token[sw.size()] == L'=');
    });
}

/// Removes proxy switches from one whitespace-separated argument string, keeping everything else verbatim.
std::wstring strip_proxy_switches(std::wstring_view text) {
    std::wstring out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::iswspace(text[i]) != 0) {
            ++i;
        }
        const std::size_t start = i;
        bool in_quotes = false;
        while (i < text.size() && (in_quotes || std::iswspace(text[i]) == 0)) {
            if (text[i] == L'"') {
                in_quotes = !in_quotes;
            }
            ++i;
        }
        const std::wstring_view token = text.substr(start, i - start);
        if (!token.empty() && !is_proxy_switch(token)) {
            if (!out.empty()) {
                out.push_back(L' ');
            }
            out += token;
        }
    }
    return out;
}

} // namespace

std::optional<std::string> validate_proxy_config(const ProxyConfig& config) {
    switch (config.mode) {
    case ProxyMode::System:
    case ProxyMode::Direct:
    case ProxyMode::AutoDetect:
        return std::nullopt;
    case ProxyMode::Fixed:
        if (config.server.empty()) {
            return "ProxyMode::Fixed requires a server";
        }
        if (has_space_or_quote(config.server)) {
            return "proxy server must not contain whitespace or quotes";
        }
        for (const auto& entry : config.bypass_list) {
            if (entry.empty() || has_space_or_quote(entry) || entry.find(L';') != std::wstring::npos) {
                return "proxy bypass entries must be non-empty and contain no whitespace, quotes or ';'";
            }
        }
        return std::nullopt;
    case ProxyMode::PacScript:
        if (config.pac_url.empty()) {
            return "ProxyMode::PacScript requires a pac_url";
        }
        if (has_space_or_quote(config.pac_url)) {
            return "PAC URL must not contain whitespace or quotes";
        }
        return std::nullopt;
    }
    return "unknown proxy mode";
}

std::vector<std::wstring> proxy_switches(const ProxyConfig& config) {
    std::vector<std::wstring> out;
    if (validate_proxy_config(config).has_value()) {
        return out;
    }
    switch (config.mode) {
    case ProxyMode::System:
        break;
    case ProxyMode::Direct:
        out.emplace_back(L"--no-proxy-server");
        break;
    case ProxyMode::AutoDetect:
        out.emplace_back(L"--proxy-auto-detect");
        break;
    case ProxyMode::Fixed: {
        out.push_back(L"--proxy-server=" + config.server);
        if (!config.bypass_list.empty()) {
            std::wstring list = L"--proxy-bypass-list=";
            for (std::size_t i = 0; i < config.bypass_list.size(); ++i) {
                if (i != 0) {
                    list.push_back(L';');
                }
                list += config.bypass_list[i];
            }
            out.push_back(std::move(list));
        }
        break;
    }
    case ProxyMode::PacScript:
        out.push_back(L"--proxy-pac-url=" + config.pac_url);
        break;
    }
    return out;
}

bool apply_proxy(core::EnvironmentConfig& env, const ProxyConfig& config) {
    if (validate_proxy_config(config).has_value()) {
        return false;
    }
    auto& args = env.additional_browser_arguments;
    for (auto& arg : args) {
        arg = strip_proxy_switches(arg);
    }
    std::erase_if(args, [](const std::wstring& arg) { return arg.empty(); });
    for (auto& sw : proxy_switches(config)) {
        args.push_back(std::move(sw));
    }
    return true;
}

} // namespace boson::webview
