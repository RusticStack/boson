#include "boson/ipc/sync_boot.hpp"

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>

#include <WebView2.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace boson::ipc {

namespace {

constexpr std::string_view kProcessKey = "process";

// Mirrors Bridge::origin_of: scheme://host[:port], lower-cased, userinfo stripped; '' for opaque URLs.
constexpr std::string_view kScriptHead = R"JS((()=>{try{const a=)JS";
constexpr std::string_view kScriptMiddle = R"JS(;const m=/^([a-z][a-z0-9+.\-]*):\/\/([^\/?#]*)/i.exec(location.href);let o='';if(m){let h=m[2];const at=h.lastIndexOf('@');if(at>=0)h=h.slice(at+1);if(h)o=(m[1]+'://'+h).toLowerCase();}if(a.indexOf('*')<0&&a.indexOf(o)<0)return;if(Object.prototype.hasOwnProperty.call(window,')JS";
constexpr std::string_view kScriptFreeze = R"JS('))return;const f=(v)=>{if(v!==null&&typeof v==='object'&&!Object.isFrozen(v)){Object.freeze(v);for(const k of Object.keys(v))f(v[k]);}return v;};Object.defineProperty(window,')JS";
constexpr std::string_view kScriptValue = R"JS(',{value:f()JS";
constexpr std::string_view kScriptTail = R"JS(),writable:false,configurable:false,enumerable:false});}catch(e){}})();)JS";

std::string lower_trimmed(std::string_view text) {
    while (!text.empty() && text.back() == '/') {
        text.remove_suffix(1);
    }
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

void append_json_string(std::string& out, std::string_view text) {
    std::string quoted;
    [[maybe_unused]] const auto ec = glz::write_json(text, quoted);
    out.append(quoted);
}

constexpr std::string_view process_arch() noexcept {
#if defined(_M_ARM64)
    return "arm64";
#elif defined(_M_X64)
    return "x64";
#else
    return "ia32";
#endif
}

} // namespace

bool BootValues::put(std::vector<Entry>& entries, std::string_view key, std::string json) {
    if (key.empty() || glz::validate_json(json)) {
        return false;
    }
    if (const auto it = std::ranges::find(entries, key, &Entry::key); it != entries.end()) {
        it->json = std::move(json);
        return true;
    }
    entries.push_back(Entry{.key = std::string{key}, .json = std::move(json)});
    return true;
}

bool BootValues::set_json(std::string_view key, std::string json) {
    if (key == kSendSyncKey) {
        return false; // owned by set_send_sync()
    }
    return put(m_values, key, std::move(json));
}

bool BootValues::set_send_sync_json(std::string_view channel, std::string json) {
    return put(m_send_sync, channel, std::move(json));
}

bool BootValues::set_process_info(std::string_view app_version) {
    std::string webview2;
    LPWSTR version = nullptr;
    if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version != nullptr) {
        webview2 = core::wide_to_utf8(version);
        ::CoTaskMemFree(version);
    }
    std::string json{R"({"platform":"win32","arch":)"};
    append_json_string(json, process_arch());
    json.append(R"(,"versions":{"webview2":)");
    append_json_string(json, webview2);
    json.append(R"(,"chrome":)");
    append_json_string(json, webview2); // WebView2 runtime = Chromium-based Edge; its major matches Chromium's
    if (!app_version.empty()) {
        json.append(R"(,"app":)");
        append_json_string(json, app_version);
    }
    json.append("}}");
    return put(m_values, kProcessKey, std::move(json));
}

bool BootValues::remove(std::string_view key) {
    if (key == kSendSyncKey) {
        const bool had = !m_send_sync.empty();
        m_send_sync.clear();
        return had;
    }
    return std::erase_if(m_values, [key](const Entry& entry) { return entry.key == key; }) != 0;
}

void BootValues::clear() noexcept {
    m_values.clear();
    m_send_sync.clear();
}

bool BootValues::contains(std::string_view key) const noexcept {
    if (key == kSendSyncKey) {
        return !m_send_sync.empty();
    }
    return std::ranges::find(m_values, key, &Entry::key) != m_values.end();
}

std::string BootValues::to_json() const {
    std::size_t total = 2;
    for (const auto& entry : m_values) {
        total += entry.key.size() + entry.json.size() + 4;
    }
    for (const auto& entry : m_send_sync) {
        total += entry.key.size() + entry.json.size() + 4;
    }
    std::string out;
    out.reserve(total + 16);
    out.push_back('{');
    bool first = true;
    const auto append_entries = [&out](const std::vector<Entry>& entries, bool& is_first) {
        for (const auto& entry : entries) {
            if (!is_first) {
                out.push_back(',');
            }
            is_first = false;
            append_json_string(out, entry.key);
            out.push_back(':');
            out.append(entry.json);
        }
    };
    append_entries(m_values, first);
    if (!m_send_sync.empty()) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        append_json_string(out, kSendSyncKey);
        out.append(":{");
        bool inner_first = true;
        append_entries(m_send_sync, inner_first);
        out.push_back('}');
    }
    out.push_back('}');
    return out;
}

std::string BootValues::build_script(const std::vector<std::string>& origins) const {
    std::string origins_json{"["};
    for (std::size_t i = 0; i < origins.size(); ++i) {
        if (i != 0) {
            origins_json.push_back(',');
        }
        append_json_string(origins_json, origins[i] == "*" ? std::string{"*"} : lower_trimmed(origins[i]));
    }
    origins_json.push_back(']');

    const std::string values = to_json(); // JSON is a valid JavaScript expression (ES2019)
    std::string script;
    script.reserve(512 + origins_json.size() + values.size());
    script.append(kScriptHead);
    script.append(origins_json);
    script.append(kScriptMiddle);
    script.append(kGlobalName);
    script.append(kScriptFreeze);
    script.append(kGlobalName);
    script.append(kScriptValue);
    script.append(values);
    script.append(kScriptTail);
    return script;
}

std::string BootValues::script() const {
    if (m_allowed_origins.empty()) {
        return build_script({std::string{"boson://app"}});
    }
    return build_script(m_allowed_origins);
}

std::string BootValues::script_for(const core::WebView& webview) const {
    if (!m_allowed_origins.empty()) {
        return build_script(m_allowed_origins);
    }
    std::vector<std::string> origins{std::string{"boson://app"}};
    if (const auto& dev = webview.config().dev_url; dev.has_value()) {
        if (std::string dev_origin = Bridge::origin_of(*dev); !dev_origin.empty()) {
            origins.push_back(std::move(dev_origin));
        }
    }
    return build_script(origins);
}

std::wstring BootValues::wide_script() const {
    return core::utf8_to_wide(script());
}

bool BootValues::install(core::WebView& webview, bool run_now) {
    if (!webview.is_ready()) {
        return false;
    }
    uninstall(webview);
    const std::string js = script_for(webview);
    auto state = std::make_shared<ScriptState>();
    Microsoft::WRL::ComPtr<ICoreWebView2> core{webview.core_webview()};
    const bool added = webview.add_script_on_document_created(js, [state, core](HRESULT hr, std::string id) {
        if (!core::hr_check(hr, "BootValues: AddScriptToExecuteOnDocumentCreated")) {
            return;
        }
        if (state->removed) {
            const std::wstring wide = core::utf8_to_wide(id);
            core::hr_check(core->RemoveScriptToExecuteOnDocumentCreated(wide.c_str()), "BootValues: RemoveScriptToExecuteOnDocumentCreated");
            return;
        }
        state->id = std::move(id);
    });
    if (!added) {
        return false;
    }
    m_installed.push_back(Installed{.webview_id = webview.id(), .state = std::move(state)});
    if (run_now) {
        webview.execute_script(js, [](HRESULT hr, std::string) { core::hr_check(hr, "BootValues: ExecuteScript"); });
    }
    return true;
}

bool BootValues::uninstall(core::WebView& webview) {
    const auto it = std::ranges::find(m_installed, webview.id(), &Installed::webview_id);
    if (it == m_installed.end()) {
        return false;
    }
    it->state->removed = true;
    if (!it->state->id.empty()) {
        webview.remove_script_on_document_created(it->state->id);
    }
    m_installed.erase(it);
    return true;
}

} // namespace boson::ipc
