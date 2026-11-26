#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "boson/core/webview.hpp"

namespace boson::ipc {

/// @brief Values the page can read synchronously with zero IPC: a frozen window.__BOSON_BOOT__ object injected
/// with AddScriptToExecuteOnDocumentCreated, so it exists before any page script runs. This is the recommended
/// replacement for most ipcRenderer.sendSync uses (config, paths, feature flags, platform info): computed once in
/// C++, read in nanoseconds in JS (frontend: boson.boot(key); Electron shim: zero-argument sendSync(channel) is
/// answered from set_send_sync() values first).
///
/// Values are deep-frozen. The script only defines the object when the document's origin is allowed (default
/// boson://app plus the WebView's dev_url origin), so third-party iframes and navigations never see it. Changing
/// a value after install() affects documents created after the next install() call only.
/// Not thread-safe: build it on one thread; install() / uninstall() on the UI thread.
class BootValues {
public:
    static constexpr std::string_view kGlobalName = "__BOSON_BOOT__";
    /// Sub-object read by the Electron shim's sendSync(channel) fast path.
    static constexpr std::string_view kSendSyncKey = "sendSync";

    /// @brief Serializes value with glaze. false on serialization failure or an empty / reserved key.
    template <typename T>
    bool set(std::string_view key, const T& value) {
        std::string json;
        if (glz::write_json(value, json)) {
            return false;
        }
        return set_json(key, std::move(json));
    }
    /// @brief Stores pre-serialized JSON (validated). false for invalid JSON or an empty / reserved key.
    bool set_json(std::string_view key, std::string json);

    /// @brief Precomputed reply of ipcRenderer.sendSync(channel) called without arguments.
    template <typename T>
    bool set_send_sync(std::string_view channel, const T& value) {
        std::string json;
        if (glz::write_json(value, json)) {
            return false;
        }
        return set_send_sync_json(channel, std::move(json));
    }
    bool set_send_sync_json(std::string_view channel, std::string json);

    /// @brief "process": {platform:"win32", arch, versions:{webview2, chrome, app}} for Electron's process stub.
    bool set_process_info(std::string_view app_version = {});

    bool remove(std::string_view key);
    void clear() noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_values.size() + (m_send_sync.empty() ? 0U : 1U); }

    /// @brief Origins (scheme://host[:port]) whose documents get the object; "*" = every origin. Empty (default) =
    /// boson://app plus, for install(), the origin of the WebView's dev_url.
    void set_allowed_origins(std::vector<std::string> origins) { m_allowed_origins = std::move(origins); }

    /// @brief The object as JSON, keys in insertion order (sendSync last).
    [[nodiscard]] std::string to_json() const;
    /// @brief The document-created script for the configured origins (no dev_url: use script_for()).
    [[nodiscard]] std::string script() const;
    /// @brief The script with the default origins of webview (boson://app + its dev_url origin).
    [[nodiscard]] std::string script_for(const core::WebView& webview) const;
    /// @brief script() as UTF-16, for WebViewConfig::scripts_on_document_created (survives Environment::recreate).
    [[nodiscard]] std::wstring wide_script() const;

    /// @brief Adds script_for(webview) as a document-created script, replacing what this object installed on
    /// that WebView before. run_now also defines it in the current document (if not defined yet). UI thread.
    bool install(core::WebView& webview, bool run_now = false);
    /// @brief Removes the script this object installed on webview. UI thread.
    bool uninstall(core::WebView& webview);

private:
    struct Entry {
        std::string key;
        std::string json;
    };
    struct ScriptState {
        std::string id;
        bool removed = false;
    };
    struct Installed {
        std::uint64_t webview_id = 0;
        std::shared_ptr<ScriptState> state;
    };

    [[nodiscard]] std::string build_script(const std::vector<std::string>& origins) const;
    static bool put(std::vector<Entry>& entries, std::string_view key, std::string json);

    std::vector<Entry> m_values;
    std::vector<Entry> m_send_sync;
    std::vector<std::string> m_allowed_origins;
    std::vector<Installed> m_installed;
};

} // namespace boson::ipc
