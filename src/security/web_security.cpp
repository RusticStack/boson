#include <boson/security/web_security.hpp>

#include <boson/core/result.hpp>
#include <boson/security/common.hpp>

#include <shellapi.h>

#include <memory>
#include <utility>

namespace boson::security {

namespace {

bool has_crlf(std::string_view text) noexcept {
    return text.find_first_of("\r\n") != std::string_view::npos;
}

void append_sources(std::string& out, const std::vector<std::string>& sources) {
    for (const auto& source : sources) {
        if (!source.empty() && !has_crlf(source) && source.find(';') == std::string::npos) {
            out.push_back(' ');
            out.append(source);
        }
    }
}

std::string_view scheme_of(std::string_view uri) noexcept {
    const auto colon = uri.find(':');
    return (colon == std::string_view::npos || colon == 0) ? std::string_view{} : uri.substr(0, colon);
}

struct DevSources {
    std::string origin; ///< "http://localhost:5173"
    std::string socket; ///< "ws://localhost:5173"
};

std::optional<DevSources> dev_sources(const std::optional<std::string>& dev_url) {
    if (!dev_url.has_value()) {
        return std::nullopt;
    }
    std::string origin = normalized_origin(*dev_url);
    if (origin.empty()) {
        return std::nullopt;
    }
    DevSources out{.origin = origin};
    if (origin.starts_with("https://")) {
        out.socket = "wss://" + origin.substr(8);
    } else if (origin.starts_with("http://")) {
        out.socket = "ws://" + origin.substr(7);
    }
    return out;
}

std::string build_csp(const SecurityHeaders& options, const std::optional<std::string>& dev_url) {
    const auto dev = dev_sources(dev_url);
    const auto with_dev = [&dev](std::string& out, bool socket) {
        if (dev.has_value()) {
            out.push_back(' ');
            out.append(dev->origin);
            if (socket && !dev->socket.empty()) {
                out.push_back(' ');
                out.append(dev->socket);
            }
        }
    };
    std::string csp = "default-src 'self'";
    with_dev(csp, false);
    csp.append("; script-src 'self' 'wasm-unsafe-eval'");
    with_dev(csp, false);
    append_sources(csp, options.extra_script_src);
    csp.append("; style-src 'self' 'unsafe-inline'");
    with_dev(csp, false);
    csp.append("; img-src 'self' data: blob:");
    with_dev(csp, false);
    append_sources(csp, options.extra_img_src);
    csp.append("; font-src 'self' data:");
    with_dev(csp, false);
    csp.append("; connect-src 'self'");
    with_dev(csp, true);
    append_sources(csp, options.extra_connect_src);
    csp.append("; media-src 'self' blob:; worker-src 'self' blob:; object-src 'none'; base-uri 'self'; "
               "form-action 'self'; frame-ancestors 'none'");
    return csp;
}

void append_header(std::string& out, std::string_view name, std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (!out.empty()) {
        out.append("\r\n");
    }
    out.append(name);
    out.append(": ");
    out.append(value);
}

} // namespace

std::string default_content_security_policy(const std::optional<std::string>& dev_url) {
    return build_csp(SecurityHeaders{}, dev_url);
}

std::string build_security_headers(const SecurityHeaders& options, const std::optional<std::string>& dev_url) {
    std::string out;
    if (options.csp_enabled) {
        const std::string csp = options.content_security_policy.empty() ? build_csp(options, dev_url)
                                                                        : options.content_security_policy;
        append_header(out, options.report_only ? "Content-Security-Policy-Report-Only" : "Content-Security-Policy", csp);
    }
    if (options.nosniff) {
        append_header(out, "X-Content-Type-Options", "nosniff");
    }
    append_header(out, "Referrer-Policy", options.referrer_policy);
    append_header(out, "Permissions-Policy", options.permissions_policy);
    append_header(out, "Cross-Origin-Opener-Policy", options.cross_origin_opener_policy);
    for (const auto& line : options.extra_headers) {
        if (!line.empty()) {
            if (!out.empty()) {
                out.append("\r\n");
            }
            out.append(line);
        }
    }
    return out;
}

bool apply_security_headers(core::WebViewConfig& config, const SecurityHeaders& options) {
    const auto bad = [](std::string_view text) { return has_crlf(text); };
    if (bad(options.content_security_policy) || bad(options.referrer_policy) || bad(options.permissions_policy) ||
        bad(options.cross_origin_opener_policy)) {
        core::hr_check(E_INVALIDARG, "apply_security_headers: CR/LF in a header value");
        return false;
    }
    for (const auto& line : options.extra_headers) {
        if (bad(line) || line.find(':') == std::string::npos) {
            core::hr_check(E_INVALIDARG, "apply_security_headers: malformed extra header line");
            return false;
        }
    }
    config.asset_response_headers = build_security_headers(options, config.dev_url);
    return true;
}

// ---- Navigation ----------------------------------------------------------------------------------------

CompiledNavigationPolicy::CompiledNavigationPolicy(const NavigationPolicy& policy, const std::optional<std::string>& dev_url)
    : m_external_schemes(policy.external_schemes),
      m_otherwise(policy.otherwise),
      m_allow_about_blank(policy.allow_about_blank) {
    const std::vector<std::string> allowed =
        policy.allowed_origins.empty() ? default_app_origins(dev_url) : policy.allowed_origins;
    m_allowed = compile_origin_patterns(allowed);
    m_external = compile_origin_patterns(policy.external_origins);
}

bool CompiledNavigationPolicy::is_external_scheme_allowed(std::string_view uri) const noexcept {
    const std::string_view scheme = scheme_of(uri);
    for (const auto& allowed : m_external_schemes) {
        if (iequals_ascii(scheme, allowed)) {
            return true;
        }
    }
    return false;
}

NavigationAction CompiledNavigationPolicy::decide(std::string_view uri) const noexcept {
    if (m_allow_about_blank && iequals_ascii(uri, "about:blank")) {
        return NavigationAction::Allow;
    }
    if (matches_any(m_allowed, uri)) {
        return NavigationAction::Allow;
    }
    NavigationAction action = matches_any(m_external, uri) ? NavigationAction::OpenExternal : m_otherwise;
    if (action == NavigationAction::OpenExternal && !is_external_scheme_allowed(uri)) {
        action = NavigationAction::Block;
    }
    return action;
}

bool open_external_uri(std::string_view uri) noexcept {
    const std::string_view scheme = scheme_of(uri);
    if (!iequals_ascii(scheme, "https") && !iequals_ascii(scheme, "http") && !iequals_ascii(scheme, "mailto")) {
        return false;
    }
    if (uri.find_first_of("\r\n\"") != std::string_view::npos) {
        return false;
    }
    try {
        const std::wstring wide = core::utf8_to_wide(uri);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOZONECHECKS;
        info.lpVerb = L"open";
        info.lpFile = wide.c_str();
        info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) != FALSE;
    } catch (...) {
        return false;
    }
}

void install_navigation_policy(core::WebView& webview, NavigationPolicy policy) {
    struct State {
        CompiledNavigationPolicy compiled;
        std::function<void(std::string_view)> open_external;
        std::function<void(std::string_view, NavigationAction)> on_rejected;
        std::function<void(core::NavigationStartingEvent&)> previous;
    };
    auto state = std::make_shared<State>(State{
        .compiled = CompiledNavigationPolicy(policy, webview.config().dev_url),
        .open_external = std::move(policy.open_external),
        .on_rejected = std::move(policy.on_rejected),
        .previous = std::move(webview.events().on_navigation_starting),
    });
    webview.events().on_navigation_starting = [state](core::NavigationStartingEvent& event) {
        const NavigationAction action = state->compiled.decide(event.uri);
        if (action == NavigationAction::Allow) {
            if (state->previous) {
                state->previous(event);
            }
            return;
        }
        event.cancel = true;
        if (action == NavigationAction::OpenExternal) {
            if (state->open_external) {
                state->open_external(event.uri);
            } else if (!open_external_uri(event.uri)) {
                core::hr_check(E_FAIL, "navigation policy: ShellExecuteExW failed");
            }
        }
        if (state->on_rejected) {
            state->on_rejected(event.uri, action);
        }
    };
}

// ---- New windows ---------------------------------------------------------------------------------------

NewWindowAction decide_new_window(
    std::span<const OriginPattern> allowed,
    std::span<const OriginPattern> external,
    NewWindowAction allowed_action,
    NewWindowAction otherwise,
    std::string_view uri) noexcept {
    if (matches_any(allowed, uri)) {
        return allowed_action;
    }
    if (matches_any(external, uri)) {
        const std::string_view scheme = scheme_of(uri);
        if (iequals_ascii(scheme, "https") || iequals_ascii(scheme, "http") || iequals_ascii(scheme, "mailto")) {
            return NewWindowAction::OpenExternal;
        }
        return NewWindowAction::Block;
    }
    return otherwise;
}

void install_new_window_policy(core::WebView& webview, NewWindowPolicy policy) {
    struct State {
        std::vector<OriginPattern> allowed;
        std::vector<OriginPattern> external;
        NewWindowAction allowed_action;
        NewWindowAction otherwise;
        std::function<void(std::string_view)> open_external;
        std::function<void(std::string_view, NewWindowAction)> on_rejected;
        std::function<void(core::NewWindowRequest&)> previous;
        core::WebView* webview;
    };
    auto state = std::make_shared<State>(State{
        .allowed = compile_origin_patterns(policy.allowed_origins),
        .external = compile_origin_patterns(policy.external_origins),
        .allowed_action = policy.allowed_action == NewWindowAction::NavigateSelf ? NewWindowAction::NavigateSelf
                                                                                 : NewWindowAction::AllowDefault,
        .otherwise = policy.otherwise,
        .open_external = std::move(policy.open_external),
        .on_rejected = std::move(policy.on_rejected),
        .previous = std::move(webview.events().on_new_window_requested),
        .webview = &webview,
    });
    webview.events().on_new_window_requested = [state](core::NewWindowRequest& request) {
        const NewWindowAction action =
            decide_new_window(state->allowed, state->external, state->allowed_action, state->otherwise, request.uri());
        switch (action) {
        case NewWindowAction::AllowDefault:
            if (state->previous) {
                state->previous(request);
            }
            return;
        case NewWindowAction::NavigateSelf:
            request.set_handled();
            state->webview->navigate(request.uri());
            return;
        case NewWindowAction::OpenExternal:
            request.set_handled();
            if (state->open_external) {
                state->open_external(request.uri());
            } else if (!open_external_uri(request.uri())) {
                core::hr_check(E_FAIL, "new-window policy: ShellExecuteExW failed");
            }
            break;
        case NewWindowAction::Block:
            request.set_handled();
            break;
        }
        if (state->on_rejected) {
            state->on_rejected(request.uri(), action);
        }
    };
}

} // namespace boson::security
