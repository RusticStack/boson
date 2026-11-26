#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/security/common.hpp>
#include <boson/security/origin.hpp>

namespace boson::security {

/// @brief One capability (Tauri v2 style): which IPC methods a set of origins may call.
/// Method patterns: "Controller.method", "Controller.*", "Controller.prefix*" or "*".
struct Capability {
    std::string identifier;
    std::string description;
    /// Origin patterns (see OriginPattern). Empty = every origin the Bridge already accepts.
    std::vector<std::string> origins;
    std::vector<std::string> allow;
    /// Deny always wins over allow, across all capabilities that match the origin.
    std::vector<std::string> deny;
};

struct CapabilityConfig {
    /// true: a method no capability allows is rejected. false: only explicit deny entries reject.
    bool default_deny = true;
    std::vector<Capability> capabilities;
};

enum class PermissionDecision : std::uint8_t {
    Allowed,
    DeniedByRule, ///< an explicit deny pattern matched
    NotGranted,   ///< default_deny and no allow pattern matched
    Malformed,    ///< the request has no readable "Controller.method"
};

/// JSON-RPC error code returned to the frontend promise for a rejected call.
inline constexpr int kPermissionDeniedCode = -32003;

/// @brief id + method of a raw request, as views into the JSON buffer (no allocation).
struct RpcHeaderView {
    std::uint64_t id = 0;
    std::string_view method;
};

/// @brief Reads only id and method (params are skipped). nullopt when they cannot be read without
/// allocating (malformed JSON or an escaped method string) - callers must then deny.
[[nodiscard]] std::optional<RpcHeaderView> peek_rpc_header(std::string_view raw_json) noexcept;

/// @brief Compiled, immutable permission set. check() never allocates; share one instance between
/// WebViews with std::shared_ptr<const CapabilityPolicy>.
class CapabilityPolicy {
public:
    /// Default-deny with no capabilities: every call is rejected.
    CapabilityPolicy() = default;

    /// @brief Validates and compiles config. Fails on an invalid method or origin pattern.
    [[nodiscard]] static SecurityResult<CapabilityPolicy> compile(CapabilityConfig config);
    /// @brief Parses {"default_deny":true,"capabilities":[{"identifier":..,"origins":[..],"allow":[..],"deny":[..]}]}.
    [[nodiscard]] static SecurityResult<CapabilityPolicy> from_json(std::string_view json);
    /// @brief compile() + std::make_shared, for BridgeAttachOptions::capabilities.
    [[nodiscard]] static SecurityResult<std::shared_ptr<const CapabilityPolicy>> make_shared(CapabilityConfig config);
    [[nodiscard]] static SecurityResult<std::shared_ptr<const CapabilityPolicy>> shared_from_json(std::string_view json);
    /// @brief Allows everything (default_deny = false, no rules).
    [[nodiscard]] static std::shared_ptr<const CapabilityPolicy> allow_all();

    /// @brief Decides whether source_uri (a page URI or bare origin) may call method ("Controller.method").
    [[nodiscard]] PermissionDecision check(std::string_view method, std::string_view source_uri) const noexcept;
    [[nodiscard]] bool is_allowed(std::string_view method, std::string_view source_uri) const noexcept {
        return check(method, source_uri) == PermissionDecision::Allowed;
    }

    [[nodiscard]] bool default_deny() const noexcept { return m_default_deny; }
    [[nodiscard]] const CapabilityConfig& config() const noexcept { return m_config; }
    [[nodiscard]] std::string to_json() const;

    /// @brief true when text is a valid method pattern.
    [[nodiscard]] static bool is_valid_method_pattern(std::string_view text) noexcept;

private:
    struct MethodSet {
        std::vector<std::string> exact;    ///< sorted, binary-searched
        std::vector<std::string> prefixes; ///< from "X.*" / "X.pre*"; "" = everything
        [[nodiscard]] bool matches(std::string_view method) const noexcept;
        void add(std::string_view pattern);
        void finish();
    };
    struct Compiled {
        bool any_origin = true;
        std::vector<OriginPattern> origins;
        MethodSet allow;
        MethodSet deny;
    };

    std::vector<Compiled> m_rules;
    bool m_default_deny = true;
    CapabilityConfig m_config;
};

/// @brief {"id":id,"status":"error","error":{"code":-32003,"message":"permission denied: <method> (...)"}}.
[[nodiscard]] std::string make_permission_denied_envelope(
    std::uint64_t id, std::string_view method, PermissionDecision decision);

[[nodiscard]] std::string_view to_string(PermissionDecision decision) noexcept;

} // namespace boson::security
