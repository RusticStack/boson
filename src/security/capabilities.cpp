#include <boson/security/capabilities.hpp>

#include <boson/ipc/protocol_types.hpp>

#include <windows.h>

#include <algorithm>
#include <functional>

namespace boson::security {

namespace {

bool is_identifier_char(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

bool is_identifier(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, is_identifier_char);
}

SecurityError invalid(const Capability& capability, std::string_view what, std::string_view value) {
    std::string message = "capability '" + capability.identifier + "': invalid ";
    message.append(what);
    message.append(" '");
    message.append(value);
    message.push_back('\'');
    return SecurityError{.code = ERROR_INVALID_PARAMETER, .message = std::move(message)};
}

} // namespace

std::optional<RpcHeaderView> peek_rpc_header(std::string_view raw_json) noexcept {
    RpcHeaderView header{};
    const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(header, raw_json);
    if (ec || header.method.empty() || header.method.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }
    return header;
}

bool CapabilityPolicy::is_valid_method_pattern(std::string_view text) noexcept {
    if (text == "*") {
        return true;
    }
    const auto dot = text.find('.');
    if (dot == std::string_view::npos || !is_identifier(text.substr(0, dot))) {
        return false;
    }
    std::string_view method = text.substr(dot + 1);
    if (method == "*") {
        return true;
    }
    if (method.ends_with('*')) {
        method.remove_suffix(1);
    }
    return is_identifier(method);
}

void CapabilityPolicy::MethodSet::add(std::string_view pattern) {
    if (pattern.ends_with('*')) {
        pattern.remove_suffix(1);
        prefixes.emplace_back(pattern);
    } else {
        exact.emplace_back(pattern);
    }
}

void CapabilityPolicy::MethodSet::finish() {
    std::ranges::sort(exact);
    exact.erase(std::ranges::unique(exact).begin(), exact.end());
    std::ranges::sort(prefixes);
    prefixes.erase(std::ranges::unique(prefixes).begin(), prefixes.end());
}

bool CapabilityPolicy::MethodSet::matches(std::string_view method) const noexcept {
    const auto it = std::ranges::lower_bound(exact, method, std::less<>{});
    if (it != exact.end() && std::string_view(*it) == method) {
        return true;
    }
    for (const auto& prefix : prefixes) {
        if (method.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

bool CapabilityPolicy::is_valid_window_pattern(std::string_view text) noexcept {
    if (text == "*") {
        return true;
    }
    if (text.ends_with('*')) {
        text.remove_suffix(1);
    }
    return !text.empty() && text.size() <= 64 && std::ranges::all_of(text, [](char ch) {
        return is_identifier_char(ch) || ch == ':' || ch == '/' || ch == '.';
    });
}

bool CapabilityPolicy::may_create_window(std::string_view label, std::string_view source_uri) const noexcept {
    const OriginParts origin = parse_origin(source_uri);
    for (const auto& rule : m_rules) {
        bool origin_ok = rule.any_origin;
        for (std::size_t i = 0; !origin_ok && i < rule.origins.size(); ++i) {
            origin_ok = rule.origins[i].matches(origin);
        }
        if (origin_ok && rule.windows.matches(label)) {
            return true;
        }
    }
    return false;
}

SecurityResult<CapabilityPolicy> CapabilityPolicy::compile(CapabilityConfig config) {
    CapabilityPolicy policy;
    policy.m_default_deny = config.default_deny;
    policy.m_rules.reserve(config.capabilities.size());
    for (const auto& capability : config.capabilities) {
        Compiled rule;
        rule.any_origin = capability.origins.empty();
        for (const auto& origin : capability.origins) {
            OriginPattern pattern(origin);
            if (!pattern.valid()) {
                return std::unexpected(invalid(capability, "origin pattern", origin));
            }
            rule.origins.push_back(std::move(pattern));
        }
        for (const auto& pattern : capability.allow) {
            if (!is_valid_method_pattern(pattern)) {
                return std::unexpected(invalid(capability, "allow pattern", pattern));
            }
            rule.allow.add(pattern);
        }
        for (const auto& pattern : capability.deny) {
            if (!is_valid_method_pattern(pattern)) {
                return std::unexpected(invalid(capability, "deny pattern", pattern));
            }
            rule.deny.add(pattern);
        }
        for (const auto& pattern : capability.windows) {
            if (!is_valid_window_pattern(pattern)) {
                return std::unexpected(invalid(capability, "window pattern", pattern));
            }
            rule.windows.add(pattern);
        }
        rule.allow.finish();
        rule.deny.finish();
        rule.windows.finish();
        policy.m_rules.push_back(std::move(rule));
    }
    policy.m_config = std::move(config);
    return policy;
}

SecurityResult<CapabilityPolicy> CapabilityPolicy::from_json(std::string_view json) {
    CapabilityConfig config{};
    const std::string buffer(json);
    const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(config, buffer);
    if (ec) {
        return std::unexpected(SecurityError{
            .code = ERROR_INVALID_DATA,
            .message = "capability JSON: " + glz::format_error(ec, buffer)});
    }
    return compile(std::move(config));
}

SecurityResult<std::shared_ptr<const CapabilityPolicy>> CapabilityPolicy::make_shared(CapabilityConfig config) {
    auto compiled = compile(std::move(config));
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    return std::make_shared<const CapabilityPolicy>(std::move(*compiled));
}

SecurityResult<std::shared_ptr<const CapabilityPolicy>> CapabilityPolicy::shared_from_json(std::string_view json) {
    auto compiled = from_json(json);
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    return std::make_shared<const CapabilityPolicy>(std::move(*compiled));
}

std::shared_ptr<const CapabilityPolicy> CapabilityPolicy::allow_all() {
    CapabilityPolicy policy;
    policy.m_default_deny = false;
    policy.m_config.default_deny = false;
    return std::make_shared<const CapabilityPolicy>(std::move(policy));
}

PermissionDecision CapabilityPolicy::check(std::string_view method, std::string_view source_uri) const noexcept {
    const auto dot = method.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= method.size()) {
        return PermissionDecision::Malformed;
    }
    const OriginParts origin = parse_origin(source_uri);
    bool allowed = false;
    for (const auto& rule : m_rules) {
        bool origin_ok = rule.any_origin;
        for (std::size_t i = 0; !origin_ok && i < rule.origins.size(); ++i) {
            origin_ok = rule.origins[i].matches(origin);
        }
        if (!origin_ok) {
            continue;
        }
        if (rule.deny.matches(method)) {
            return PermissionDecision::DeniedByRule;
        }
        allowed = allowed || rule.allow.matches(method);
    }
    if (allowed || !m_default_deny) {
        return PermissionDecision::Allowed;
    }
    return PermissionDecision::NotGranted;
}

std::string CapabilityPolicy::to_json() const {
    std::string out;
    [[maybe_unused]] const auto ec = glz::write_json(m_config, out);
    return out;
}

std::string_view to_string(PermissionDecision decision) noexcept {
    switch (decision) {
    case PermissionDecision::Allowed:
        return "allowed";
    case PermissionDecision::DeniedByRule:
        return "denied by capability rule";
    case PermissionDecision::NotGranted:
        return "not granted by any capability";
    case PermissionDecision::Malformed:
        return "malformed request";
    }
    return "denied";
}

std::string make_permission_denied_envelope(std::uint64_t id, std::string_view method, PermissionDecision decision) {
    std::string message = "permission denied: ";
    message.append(method.empty() ? std::string_view{"<unknown>"} : method);
    message.append(" (");
    message.append(to_string(decision));
    message.push_back(')');
    return ipc::make_error_envelope(id, kPermissionDeniedCode, message);
}

} // namespace boson::security
