#pragma once

#include <windows.h>

#include <boson/core/webview_events.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::core {
class WebView;
} // namespace boson::core

namespace boson::native {

/// @brief What the policy says for one (origin, permission kind).
enum class PermissionDecision : std::uint8_t {
    Allow,
    Deny,
    Ask,     ///< run the prompt handler (a JS prompt, a native dialog...); Default when there is none
    Default, ///< leave the request alone: WebView2 shows its own permission prompt
};

[[nodiscard]] std::string_view to_string(PermissionDecision decision) noexcept;
[[nodiscard]] std::optional<PermissionDecision> parse_permission_decision(std::string_view name) noexcept;
/// "microphone", "camera", "geolocation", "notifications", "other_sensors", "clipboard_read",
/// "multiple_automatic_downloads", "file_read_write", "autoplay", "local_fonts", "midi_sysex",
/// "window_management", "persistent_storage", "unknown".
[[nodiscard]] std::string_view to_string(core::PermissionKind kind) noexcept;
[[nodiscard]] std::optional<core::PermissionKind> parse_permission_kind(std::string_view name) noexcept;

/// @brief One remembered decision (only Allow / Deny are stored). origin is "scheme://host[:port]", lower-case.
struct PermissionRule {
    std::string origin;
    std::string kind;
    std::string decision;
};

/// @brief A request waiting for the prompt handler.
struct PermissionPrompt {
    std::uint64_t id = 0;
    std::uint64_t webview_id = 0;
    std::string origin;
    std::string uri;
    core::PermissionKind kind = core::PermissionKind::Unknown;
    bool user_initiated = false;
};

/// @brief Per-origin permission policy for WebView2 PermissionRequested, persisted to a small JSON file.
///
/// Resolution order for a request: a remembered rule for (origin, kind), then the per-kind default
/// (set_default), then the global fallback (Ask). Ask runs the prompt handler (asynchronously: the WebView2
/// request is deferred until answer()); without a handler it degrades to Default. Allow / Deny answers set
/// SavesInProfile = false, so the WebView2 profile never caches a decision behind the policy's back.
///
/// Thread safety: policy members (set_default, set, reset, clear, resolve, rules, load, save) may be called
/// from any thread. attach / detach / answer / pending and every callback run on the UI thread.
class PermissionManager {
public:
    using PromptHandler = std::function<void(const PermissionPrompt& prompt)>;
    using ChangeFn = std::function<void()>;

    /// @param storage JSON file for remembered rules; empty = in memory only. Loaded lazily on first use.
    explicit PermissionManager(std::filesystem::path storage = {});
    ~PermissionManager();

    PermissionManager(const PermissionManager&) = delete;
    PermissionManager& operator=(const PermissionManager&) = delete;
    PermissionManager(PermissionManager&&) = delete;
    PermissionManager& operator=(PermissionManager&&) = delete;

    // ---- Policy (any thread) ---------------------------------------------------------------------------
    void set_default(core::PermissionKind kind, PermissionDecision decision);
    void set_fallback(PermissionDecision decision);
    /// @brief Remembers Allow / Deny for origin (a URI or bare origin); Ask / Default forget the rule.
    bool set(std::string_view origin, core::PermissionKind kind, PermissionDecision decision);
    bool reset(std::string_view origin, core::PermissionKind kind);
    /// @brief Forgets every rule of origin; empty origin = every rule.
    bool clear(std::string_view origin = {});
    [[nodiscard]] PermissionDecision resolve(std::string_view origin_or_uri, core::PermissionKind kind) const;
    [[nodiscard]] std::optional<PermissionDecision> rule(std::string_view origin_or_uri, core::PermissionKind kind) const;
    [[nodiscard]] std::vector<PermissionRule> rules() const;
    /// @brief Re-reads the storage file (false when it exists but cannot be parsed; rules are then empty).
    bool load();
    bool save() const;
    [[nodiscard]] const std::filesystem::path& storage_path() const noexcept { return m_storage; }
    /// @brief Called (on the thread that made the change) after any rule change.
    void set_change_callback(ChangeFn fn);

    // ---- WebView integration (UI thread) ---------------------------------------------------------------
    /// @brief Takes over webview.events().on_permission_requested. A handler that was already installed is
    /// chained: it still runs for requests the policy resolves to Default.
    void attach(core::WebView& webview);
    void detach(core::WebView& webview) noexcept;
    /// @brief Called for every Ask request; reply later with answer(prompt.id, ...). Unset = Default.
    void set_prompt_handler(PromptHandler handler);
    /// @brief Completes a pending prompt. remember stores Allow / Deny as a rule. false = unknown id.
    bool answer(std::uint64_t prompt_id, PermissionDecision decision, bool remember);
    [[nodiscard]] std::vector<PermissionPrompt> pending() const;

    /// @brief The decision for one request (the path attach() uses). Returns false when the policy resolved to
    /// Default and left the request untouched. Exposed for custom wiring.
    bool handle_request(std::uint64_t webview_id, core::PermissionRequest& request);

private:
    struct Pending {
        PermissionPrompt prompt;
        std::function<void(core::PermissionState)> complete;
    };
    struct Attachment {
        core::WebView* webview = nullptr;
        std::function<void(core::PermissionRequest&)> previous;
    };

    void ensure_loaded() const;
    bool save_locked() const;
    void notify_changed();

    std::filesystem::path m_storage;
    mutable std::mutex m_mutex;
    mutable bool m_loaded = false;
    mutable std::map<std::pair<std::string, core::PermissionKind>, PermissionDecision> m_rules;
    std::map<core::PermissionKind, PermissionDecision> m_defaults;
    PermissionDecision m_fallback = PermissionDecision::Ask;
    ChangeFn m_on_change;

    // UI thread only.
    std::vector<Attachment> m_attachments;
    PromptHandler m_prompt;
    std::vector<Pending> m_pending;
    std::uint64_t m_next_prompt = 1;
};

} // namespace boson::native
