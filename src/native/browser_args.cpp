#include <boson/native/browser_args.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace boson::native::browser_args {

namespace {

/// Matches "--name" and "--name=..." exactly (no prefix matches such as --name-other).
[[nodiscard]] bool is_switch(std::wstring_view arg, std::wstring_view name) noexcept {
    if (!arg.starts_with(L"--")) {
        return false;
    }
    arg.remove_prefix(2);
    if (!arg.starts_with(name)) {
        return false;
    }
    arg.remove_prefix(name.size());
    return arg.empty() || arg.front() == L'=';
}

[[nodiscard]] std::wstring unquote(std::wstring_view value) {
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return std::wstring(value);
}

[[nodiscard]] std::vector<std::wstring> split_spaces(std::wstring_view text) {
    std::vector<std::wstring> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == L' ') {
            ++pos;
        }
        const std::size_t end = std::min(text.find(L' ', pos), text.size());
        if (end > pos) {
            out.emplace_back(text.substr(pos, end - pos));
        }
        pos = end;
    }
    return out;
}

void append_unique(std::vector<std::wstring>& list, std::initializer_list<std::wstring_view> items) {
    for (const std::wstring_view item : items) {
        if (!item.empty() && std::ranges::find(list, item) == list.end()) {
            list.emplace_back(item);
        }
    }
}

} // namespace

std::optional<std::wstring> find_switch(const core::EnvironmentConfig& config, std::wstring_view name) {
    for (auto it = config.additional_browser_arguments.rbegin(); it != config.additional_browser_arguments.rend(); ++it) {
        if (is_switch(*it, name)) {
            const std::wstring_view arg{*it};
            const std::size_t eq = arg.find(L'=');
            return eq == std::wstring_view::npos ? std::wstring{} : unquote(arg.substr(eq + 1));
        }
    }
    return std::nullopt;
}

std::size_t remove_switch(core::EnvironmentConfig& config, std::wstring_view name) {
    return std::erase_if(config.additional_browser_arguments, [name](const std::wstring& arg) { return is_switch(arg, name); });
}

void set_switch(core::EnvironmentConfig& config, std::wstring_view name, std::wstring_view value) {
    remove_switch(config, name);
    std::wstring arg = L"--";
    arg += name;
    if (!value.empty()) {
        arg += L'=';
        const bool quote = value.find(L' ') != std::wstring_view::npos && !value.starts_with(L'"');
        if (quote) {
            arg += L'"';
        }
        arg += value;
        if (quote) {
            arg += L'"';
        }
    }
    config.additional_browser_arguments.push_back(std::move(arg));
}

void set_max_active_webgl_contexts(core::EnvironmentConfig& config, std::uint32_t count) {
    set_switch(config, L"max-active-webgl-contexts", std::to_wstring(std::max<std::uint32_t>(count, 1)));
}

std::wstring js_flags(const core::EnvironmentConfig& config) {
    return find_switch(config, L"js-flags").value_or(std::wstring{});
}

void add_js_flags(core::EnvironmentConfig& config, std::initializer_list<std::wstring_view> flags) {
    std::vector<std::wstring> merged = split_spaces(js_flags(config));
    append_unique(merged, flags);
    std::wstring joined;
    for (const std::wstring& flag : merged) {
        if (!joined.empty()) {
            joined.push_back(L' ');
        }
        joined += flag;
    }
    if (joined.empty()) {
        remove_switch(config, L"js-flags");
        return;
    }
    set_switch(config, L"js-flags", joined);
}

void disable_features(core::EnvironmentConfig& config, std::initializer_list<std::wstring_view> features) {
    append_unique(config.disable_features, features);
}

void enable_features(core::EnvironmentConfig& config, std::initializer_list<std::wstring_view> features) {
    append_unique(config.enable_features, features);
}

void set_remote_debugging_port(core::EnvironmentConfig& config, std::uint16_t port) {
    set_switch(config, L"remote-debugging-port", std::to_wstring(port));
}

void set_autoplay_policy(core::EnvironmentConfig& config, AutoplayPolicy policy) {
    std::wstring_view value = L"no-user-gesture-required";
    switch (policy) {
    case AutoplayPolicy::NoUserGestureRequired: value = L"no-user-gesture-required"; break;
    case AutoplayPolicy::UserGestureRequired: value = L"user-gesture-required"; break;
    case AutoplayPolicy::DocumentUserActivationRequired: value = L"document-user-activation-required"; break;
    }
    set_switch(config, L"autoplay-policy", value);
}

void ignore_gpu_blocklist(core::EnvironmentConfig& config) {
    set_switch(config, L"ignore-gpu-blocklist");
}

void force_device_scale_factor(core::EnvironmentConfig& config, double scale) {
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"%g", scale > 0.0 ? scale : 1.0);
    set_switch(config, L"force-device-scale-factor", buffer);
}

} // namespace boson::native::browser_args
