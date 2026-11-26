#include <boson/security/origin.hpp>

#include <boson/core/result.hpp>
#include <boson/security/common.hpp>

namespace boson::security {

namespace {

std::string lower_copy(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

bool ends_with_icase(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && iequals_ascii(text.substr(text.size() - suffix.size()), suffix);
}

/// Splits an authority into host and port (IPv6 aware). false when malformed.
bool split_host_port(std::string_view authority, std::string_view& host, std::string_view& port) noexcept {
    host = authority;
    port = {};
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host = authority.substr(1, close - 1);
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return false;
            }
            port = rest.substr(1);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    return !host.empty();
}

} // namespace

OriginParts parse_origin(std::string_view uri) noexcept {
    const auto scheme_end = uri.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return {};
    }
    const std::string_view scheme = uri.substr(0, scheme_end);
    for (const char ch : scheme) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '+' || ch == '-' || ch == '.';
        if (!ok) {
            return {};
        }
    }
    std::string_view authority = uri.substr(scheme_end + 3);
    if (const auto end = authority.find_first_of("/?#\\"); end != std::string_view::npos) {
        authority = authority.substr(0, end);
    }
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    OriginParts parts{.scheme = scheme};
    if (authority.empty() || !split_host_port(authority, parts.host, parts.port)) {
        return {};
    }
    return parts;
}

std::string normalized_origin(std::string_view uri) {
    const OriginParts parts = parse_origin(uri);
    if (parts.empty()) {
        return {};
    }
    std::string origin = lower_copy(parts.scheme) + "://";
    const bool ipv6 = parts.host.find(':') != std::string_view::npos;
    if (ipv6) {
        origin += '[';
    }
    origin += lower_copy(parts.host);
    if (ipv6) {
        origin += ']';
    }
    if (!parts.port.empty()) {
        origin += ':';
        origin += parts.port;
    }
    return origin;
}

OriginPattern::OriginPattern(std::string_view pattern) : m_text(pattern) {
    while (!pattern.empty() && pattern.back() == '/') {
        pattern.remove_suffix(1);
    }
    if (pattern == "*") {
        m_kind = Kind::Any;
        return;
    }
    if (pattern.ends_with(":*") && pattern.find("://") == std::string_view::npos) {
        m_scheme = lower_copy(pattern.substr(0, pattern.size() - 2));
        m_kind = m_scheme.empty() ? Kind::Invalid : Kind::AnyHostOfScheme;
        return;
    }
    const auto scheme_end = pattern.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return;
    }
    m_scheme = lower_copy(pattern.substr(0, scheme_end));
    const std::string_view authority = pattern.substr(scheme_end + 3);
    if (authority.find_first_of("/?#@\\") != std::string_view::npos) {
        return; // patterns are origins, never paths
    }
    if (authority == "*") {
        m_kind = Kind::AnyHostOfScheme;
        return;
    }
    std::string_view host;
    std::string_view port;
    if (!split_host_port(authority, host, port)) {
        return;
    }
    m_port = std::string(port);
    if (host.starts_with("*.")) {
        if (host.size() <= 2 || host.find('*', 1) != std::string_view::npos) {
            return;
        }
        m_host = lower_copy(host.substr(1)); // ".example.com"
        m_kind = Kind::Subdomain;
        return;
    }
    if (host.find('*') != std::string_view::npos) {
        return;
    }
    m_host = lower_copy(host);
    m_kind = Kind::Exact;
}

bool OriginPattern::matches(std::string_view uri) const noexcept {
    if (m_kind == Kind::Any) {
        return true;
    }
    if (m_kind == Kind::AnyHostOfScheme) {
        // Scheme-only patterns also cover opaque URIs such as mailto:someone@example.com.
        const auto colon = uri.find(':');
        return colon != std::string_view::npos && colon != 0 && iequals_ascii(uri.substr(0, colon), m_scheme);
    }
    return matches(parse_origin(uri));
}

bool OriginPattern::matches(const OriginParts& origin) const noexcept {
    switch (m_kind) {
    case Kind::Invalid:
        return false;
    case Kind::Any:
        return true;
    case Kind::AnyHostOfScheme:
        return !origin.empty() && iequals_ascii(origin.scheme, m_scheme);
    case Kind::Exact:
    case Kind::Subdomain:
        break;
    }
    if (origin.empty() || !iequals_ascii(origin.scheme, m_scheme)) {
        return false;
    }
    if (m_port != "*" && origin.port != m_port) {
        return false;
    }
    if (m_kind == Kind::Exact) {
        return iequals_ascii(origin.host, m_host);
    }
    return origin.host.size() > m_host.size() && ends_with_icase(origin.host, m_host);
}

std::vector<OriginPattern> compile_origin_patterns(std::span<const std::string> patterns) {
    std::vector<OriginPattern> out;
    out.reserve(patterns.size());
    for (const auto& text : patterns) {
        OriginPattern pattern(text);
        if (pattern.valid()) {
            out.push_back(std::move(pattern));
        } else {
            core::hr_check(E_INVALIDARG, "security: ignored invalid origin pattern");
        }
    }
    return out;
}

bool matches_any(std::span<const OriginPattern> patterns, std::string_view uri) noexcept {
    for (const auto& pattern : patterns) {
        if (pattern.matches(uri)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> default_app_origins(const std::optional<std::string>& dev_url) {
    std::vector<std::string> out{"boson://app"};
    if (dev_url.has_value()) {
        if (std::string origin = normalized_origin(*dev_url); !origin.empty()) {
            out.push_back(std::move(origin));
        }
    }
    return out;
}

} // namespace boson::security
