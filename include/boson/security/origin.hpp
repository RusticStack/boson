#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::security {

/// @brief Components of a URI's origin as views into the URI (no allocation, no lower-casing).
struct OriginParts {
    std::string_view scheme;
    std::string_view host; ///< Without brackets for IPv6 literals.
    std::string_view port; ///< Empty when the URI has no explicit port.
    [[nodiscard]] bool empty() const noexcept { return scheme.empty(); }
};

/// @brief Splits "scheme://[userinfo@]host[:port]/..." into origin parts. Opaque URIs (about:blank, data:,
/// javascript:) and malformed input give empty parts.
[[nodiscard]] OriginParts parse_origin(std::string_view uri) noexcept;

/// @brief "scheme://host[:port]" lower-cased (IPv6 hosts bracketed); empty for opaque URIs.
[[nodiscard]] std::string normalized_origin(std::string_view uri);

/// @brief One entry of an origin allow-list.
///  - "*"                         any origin, including opaque ones
///  - "scheme://host[:port]"      exact origin (case-insensitive); a trailing '/' is ignored
///  - "scheme://*.example.com"    any sub-domain of example.com (not example.com itself)
///  - "scheme://host:*"           any port
///  - "scheme:*" / "scheme://*"   any origin with that scheme
class OriginPattern {
public:
    OriginPattern() = default;
    explicit OriginPattern(std::string_view pattern);

    [[nodiscard]] bool valid() const noexcept { return m_kind != Kind::Invalid; }
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }

    /// @brief Matches a full URI or a bare origin. Never allocates.
    [[nodiscard]] bool matches(std::string_view uri) const noexcept;
    [[nodiscard]] bool matches(const OriginParts& origin) const noexcept;

private:
    enum class Kind { Invalid, Any, AnyHostOfScheme, Exact, Subdomain };

    Kind m_kind = Kind::Invalid;
    std::string m_text;
    std::string m_scheme; ///< lower-case
    std::string m_host;   ///< lower-case; for Subdomain the suffix including the leading '.'
    std::string m_port;   ///< "" = no explicit port, "*" = any port
};

/// @brief Compiles patterns; invalid entries are dropped (and logged).
[[nodiscard]] std::vector<OriginPattern> compile_origin_patterns(std::span<const std::string> patterns);

/// @brief true when any pattern matches uri.
[[nodiscard]] bool matches_any(std::span<const OriginPattern> patterns, std::string_view uri) noexcept;

/// @brief "boson://app" plus the origin of dev_url when set: the default trusted set of a WebView.
[[nodiscard]] std::vector<std::string> default_app_origins(const std::optional<std::string>& dev_url);

} // namespace boson::security
