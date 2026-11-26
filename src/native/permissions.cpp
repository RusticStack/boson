#include <boson/native/permissions.hpp>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/security/origin.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>

namespace boson::native {

namespace detail {

/// On-disk form (glaze cannot reflect types with internal linkage).
struct PermissionFile {
    int version = 1;
    std::vector<PermissionRule> rules;
};

} // namespace detail

namespace {

using detail::PermissionFile;

struct KindName {
    core::PermissionKind kind;
    std::string_view name;
};

constexpr std::array<KindName, 14> kKindNames{{
    {core::PermissionKind::Unknown, "unknown"},
    {core::PermissionKind::Microphone, "microphone"},
    {core::PermissionKind::Camera, "camera"},
    {core::PermissionKind::Geolocation, "geolocation"},
    {core::PermissionKind::Notifications, "notifications"},
    {core::PermissionKind::OtherSensors, "other_sensors"},
    {core::PermissionKind::ClipboardRead, "clipboard_read"},
    {core::PermissionKind::MultipleAutomaticDownloads, "multiple_automatic_downloads"},
    {core::PermissionKind::FileReadWrite, "file_read_write"},
    {core::PermissionKind::Autoplay, "autoplay"},
    {core::PermissionKind::LocalFonts, "local_fonts"},
    {core::PermissionKind::MidiSystemExclusiveMessages, "midi_sysex"},
    {core::PermissionKind::WindowManagement, "window_management"},
    {core::PermissionKind::PersistentStorage, "persistent_storage"},
}};

/// A URI or a bare origin -> "scheme://host[:port]". "*" stays "*" (rule for every origin).
[[nodiscard]] std::string origin_key(std::string_view origin_or_uri) {
    if (origin_or_uri == "*") {
        return "*";
    }
    return security::normalized_origin(origin_or_uri);
}

[[nodiscard]] core::PermissionState to_state(PermissionDecision decision) noexcept {
    switch (decision) {
    case PermissionDecision::Allow: return core::PermissionState::Allow;
    case PermissionDecision::Deny: return core::PermissionState::Deny;
    case PermissionDecision::Ask:
    case PermissionDecision::Default: break;
    }
    return core::PermissionState::Default;
}

} // namespace

std::string_view to_string(PermissionDecision decision) noexcept {
    switch (decision) {
    case PermissionDecision::Allow: return "allow";
    case PermissionDecision::Deny: return "deny";
    case PermissionDecision::Ask: return "ask";
    case PermissionDecision::Default: return "default";
    }
    return "default";
}

std::optional<PermissionDecision> parse_permission_decision(std::string_view name) noexcept {
    if (name == "allow" || name == "granted" || name == "grant") return PermissionDecision::Allow;
    if (name == "deny" || name == "denied" || name == "block") return PermissionDecision::Deny;
    if (name == "ask" || name == "prompt") return PermissionDecision::Ask;
    if (name == "default") return PermissionDecision::Default;
    return std::nullopt;
}

std::string_view to_string(core::PermissionKind kind) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<core::PermissionKind> parse_permission_kind(std::string_view name) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.name == name) {
            return entry.kind;
        }
    }
    if (name == "clipboard-read") return core::PermissionKind::ClipboardRead;
    if (name == "media" || name == "audio") return core::PermissionKind::Microphone;
    if (name == "video") return core::PermissionKind::Camera;
    return std::nullopt;
}

PermissionManager::PermissionManager(std::filesystem::path storage) : m_storage(std::move(storage)) {}

PermissionManager::~PermissionManager() {
    for (Pending& pending : m_pending) {
        if (pending.complete) {
            pending.complete(core::PermissionState::Default);
        }
    }
    m_pending.clear();
    for (Attachment& attachment : m_attachments) {
        if (attachment.webview != nullptr) {
            attachment.webview->events().on_permission_requested = std::move(attachment.previous);
        }
    }
}

void PermissionManager::ensure_loaded() const {
    if (m_loaded) {
        return;
    }
    m_loaded = true;
    if (m_storage.empty()) {
        return;
    }
    std::ifstream in(m_storage, std::ios::binary);
    if (!in) {
        return;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    PermissionFile file;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(file, text)) {
        OutputDebugStringA("[Boson] PermissionManager: ignoring unreadable permission file\n");
        return;
    }
    for (const PermissionRule& rule : file.rules) {
        const auto kind = parse_permission_kind(rule.kind);
        const auto decision = parse_permission_decision(rule.decision);
        const std::string origin = origin_key(rule.origin);
        if (kind && decision && !origin.empty() &&
            (*decision == PermissionDecision::Allow || *decision == PermissionDecision::Deny)) {
            m_rules[{origin, *kind}] = *decision;
        }
    }
}

bool PermissionManager::load() {
    {
        std::scoped_lock lock(m_mutex);
        m_rules.clear();
        m_loaded = false;
        ensure_loaded();
    }
    return true;
}

bool PermissionManager::save_locked() const {
    if (m_storage.empty()) {
        return true;
    }
    PermissionFile file;
    file.rules.reserve(m_rules.size());
    for (const auto& [key, decision] : m_rules) {
        file.rules.push_back(PermissionRule{
            .origin = key.first,
            .kind = std::string(to_string(key.second)),
            .decision = std::string(to_string(decision)),
        });
    }
    std::string json;
    if (glz::write<glz::opts{.prettify = true}>(file, json)) {
        return false;
    }
    std::error_code ec;
    if (m_storage.has_parent_path()) {
        std::filesystem::create_directories(m_storage.parent_path(), ec);
    }
    std::filesystem::path temp = m_storage;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) {
            return false;
        }
    }
    if (MoveFileExW(temp.c_str(), m_storage.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "PermissionManager: MoveFileExW");
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool PermissionManager::save() const {
    std::scoped_lock lock(m_mutex);
    ensure_loaded();
    return save_locked();
}

void PermissionManager::notify_changed() {
    ChangeFn fn;
    {
        std::scoped_lock lock(m_mutex);
        fn = m_on_change;
    }
    if (fn) {
        fn();
    }
}

void PermissionManager::set_change_callback(ChangeFn fn) {
    std::scoped_lock lock(m_mutex);
    m_on_change = std::move(fn);
}

void PermissionManager::set_default(core::PermissionKind kind, PermissionDecision decision) {
    std::scoped_lock lock(m_mutex);
    m_defaults[kind] = decision;
}

void PermissionManager::set_fallback(PermissionDecision decision) {
    std::scoped_lock lock(m_mutex);
    m_fallback = decision;
}

bool PermissionManager::set(std::string_view origin, core::PermissionKind kind, PermissionDecision decision) {
    if (decision != PermissionDecision::Allow && decision != PermissionDecision::Deny) {
        return reset(origin, kind);
    }
    const std::string key = origin_key(origin);
    if (key.empty()) {
        return false;
    }
    bool ok = false;
    {
        std::scoped_lock lock(m_mutex);
        ensure_loaded();
        m_rules[{key, kind}] = decision;
        ok = save_locked();
    }
    notify_changed();
    return ok;
}

bool PermissionManager::reset(std::string_view origin, core::PermissionKind kind) {
    const std::string key = origin_key(origin);
    bool ok = false;
    {
        std::scoped_lock lock(m_mutex);
        ensure_loaded();
        if (m_rules.erase({key, kind}) == 0) {
            return true;
        }
        ok = save_locked();
    }
    notify_changed();
    return ok;
}

bool PermissionManager::clear(std::string_view origin) {
    const std::string key = origin.empty() ? std::string{} : origin_key(origin);
    bool ok = false;
    {
        std::scoped_lock lock(m_mutex);
        ensure_loaded();
        const std::size_t before = m_rules.size();
        if (key.empty()) {
            m_rules.clear();
        } else {
            std::erase_if(m_rules, [&key](const auto& entry) { return entry.first.first == key; });
        }
        if (before == m_rules.size()) {
            return true;
        }
        ok = save_locked();
    }
    notify_changed();
    return ok;
}

std::optional<PermissionDecision> PermissionManager::rule(std::string_view origin_or_uri, core::PermissionKind kind) const {
    const std::string key = origin_key(origin_or_uri);
    std::scoped_lock lock(m_mutex);
    ensure_loaded();
    if (const auto it = m_rules.find({key, kind}); it != m_rules.end()) {
        return it->second;
    }
    if (const auto it = m_rules.find({std::string("*"), kind}); it != m_rules.end()) {
        return it->second;
    }
    return std::nullopt;
}

PermissionDecision PermissionManager::resolve(std::string_view origin_or_uri, core::PermissionKind kind) const {
    if (const auto remembered = rule(origin_or_uri, kind)) {
        return *remembered;
    }
    std::scoped_lock lock(m_mutex);
    if (const auto it = m_defaults.find(kind); it != m_defaults.end()) {
        return it->second;
    }
    return m_fallback;
}

std::vector<PermissionRule> PermissionManager::rules() const {
    std::scoped_lock lock(m_mutex);
    ensure_loaded();
    std::vector<PermissionRule> out;
    out.reserve(m_rules.size());
    for (const auto& [key, decision] : m_rules) {
        out.push_back(PermissionRule{
            .origin = key.first,
            .kind = std::string(to_string(key.second)),
            .decision = std::string(to_string(decision)),
        });
    }
    return out;
}

void PermissionManager::set_prompt_handler(PromptHandler handler) {
    m_prompt = std::move(handler);
}

bool PermissionManager::handle_request(std::uint64_t webview_id, core::PermissionRequest& request) {
    const PermissionDecision decision = resolve(request.uri(), request.kind());
    switch (decision) {
    case PermissionDecision::Allow:
    case PermissionDecision::Deny:
        request.set_saves_in_profile(false);
        request.set_state(to_state(decision));
        return true;
    case PermissionDecision::Default:
        return false;
    case PermissionDecision::Ask:
        break;
    }
    if (!m_prompt) {
        return false;
    }
    request.set_saves_in_profile(false);
    Pending pending;
    pending.prompt = PermissionPrompt{
        .id = m_next_prompt++,
        .webview_id = webview_id,
        .origin = origin_key(request.uri()),
        .uri = request.uri(),
        .kind = request.kind(),
        .user_initiated = request.is_user_initiated(),
    };
    pending.complete = request.defer();
    const PermissionPrompt prompt = pending.prompt;
    m_pending.push_back(std::move(pending));
    try {
        m_prompt(prompt);
    } catch (const std::exception& ex) {
        OutputDebugStringA("[Boson] permission prompt handler threw: ");
        OutputDebugStringA(ex.what());
        OutputDebugStringA("\n");
        answer(prompt.id, PermissionDecision::Default, false);
    } catch (...) {
        answer(prompt.id, PermissionDecision::Default, false);
    }
    return true;
}

bool PermissionManager::answer(std::uint64_t prompt_id, PermissionDecision decision, bool remember) {
    const auto it = std::ranges::find_if(m_pending, [prompt_id](const Pending& p) { return p.prompt.id == prompt_id; });
    if (it == m_pending.end()) {
        return false;
    }
    Pending pending = std::move(*it);
    m_pending.erase(it);
    if (remember && (decision == PermissionDecision::Allow || decision == PermissionDecision::Deny)) {
        set(pending.prompt.origin, pending.prompt.kind, decision);
    }
    if (pending.complete) {
        pending.complete(to_state(decision));
    }
    return true;
}

std::vector<PermissionPrompt> PermissionManager::pending() const {
    std::vector<PermissionPrompt> out;
    out.reserve(m_pending.size());
    for (const Pending& p : m_pending) {
        out.push_back(p.prompt);
    }
    return out;
}

void PermissionManager::attach(core::WebView& webview) {
    if (std::ranges::any_of(m_attachments, [&webview](const Attachment& a) { return a.webview == &webview; })) {
        return;
    }
    Attachment attachment{.webview = &webview, .previous = webview.events().on_permission_requested};
    const std::uint64_t id = webview.id();
    webview.events().on_permission_requested = [this, id, previous = attachment.previous](core::PermissionRequest& request) {
        if (!handle_request(id, request) && previous) {
            previous(request);
        }
    };
    m_attachments.push_back(std::move(attachment));
}

void PermissionManager::detach(core::WebView& webview) noexcept {
    const auto it = std::ranges::find_if(m_attachments, [&webview](const Attachment& a) { return a.webview == &webview; });
    if (it == m_attachments.end()) {
        return;
    }
    webview.events().on_permission_requested = std::move(it->previous);
    m_attachments.erase(it);
    const std::uint64_t id = webview.id();
    for (auto p = m_pending.begin(); p != m_pending.end();) {
        if (p->prompt.webview_id == id) {
            if (p->complete) {
                p->complete(core::PermissionState::Default);
            }
            p = m_pending.erase(p);
        } else {
            ++p;
        }
    }
}

} // namespace boson::native
