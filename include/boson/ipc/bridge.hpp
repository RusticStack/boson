#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "boson/core/webview.hpp"
#include "boson/exec/task_runner.hpp"
#include "boson/exec/ui_marshal.hpp"
#include "boson/ipc/export.hpp"
#include "boson/ipc/protocol_types.hpp"
#include "boson/ipc/router.hpp"

namespace boson::ipc {

struct BridgeAttachOptions {
    /// Origins (scheme://host[:port]) allowed to call the bridge. Empty = boson://app plus the origin of the
    /// WebView's dev_url. "*" allows every origin (never do that for third-party content).
    std::vector<std::string> allowed_origins;
};

/// @brief Connects WebView2 messaging, TaskRunner execution and a type-erased Router, and pushes
/// backend-to-frontend events. One Bridge can serve several WebViews; each response goes back to the
/// WebView that sent the request. A WebView that is not attached never reaches the router.
class Bridge {
public:
    using ReplyFn = std::function<void(std::string response_json)>;
    /// Called on the UI thread; must eventually call reply (on the UI thread) with the response envelope.
    using DispatchCallback = std::function<void(std::string_view raw_json, ReplyFn reply)>;

    static constexpr std::uint64_t kAllWebViews = 0;

    Bridge(exec::TaskRunner& runner, exec::UiMarshal& marshal, DispatchCallback dispatch_fn);

    template <BosonController... Controllers>
    Bridge(exec::TaskRunner& runner, exec::UiMarshal& marshal, Router<Controllers...>& router)
        : Bridge(
              runner,
              marshal,
              [&router, &runner](std::string_view raw_json, ReplyFn reply) {
                  router.dispatch_async(raw_json, runner, std::move(reply));
              }) {}

    /// @brief Single-WebView convenience form: also attaches webview with default options.
    template <BosonController... Controllers>
    Bridge(core::WebView& webview, exec::TaskRunner& runner, exec::UiMarshal& marshal, Router<Controllers...>& router)
        : Bridge(runner, marshal, router) {
        attach(webview);
    }

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;
    ~Bridge();

    /// @brief Routes webview's messages through this bridge (UI thread). Replaces its message callback.
    void attach(core::WebView& webview, BridgeAttachOptions options = {});
    void detach(core::WebView& webview) noexcept;
    [[nodiscard]] bool is_attached(std::uint64_t webview_id) const noexcept;

    /// @brief Kept for the single-WebView form: forwards to the first attached WebView.
    [[nodiscard]] core::WebView::MessageCallback make_message_callback();

    /// @brief Serializes payload with glaze and posts it to every attached WebView. Any thread.
    template <typename T>
    bool emit(std::string_view event, const T& payload) {
        return emit_to(kAllWebViews, event, payload);
    }

    /// @brief Serializes payload with glaze and posts it to one WebView (by WebView::id()). Any thread.
    template <typename T>
    bool emit_to(std::uint64_t webview_id, std::string_view event, const T& payload) {
        std::string data;
        if (glz::write_json(payload, data)) {
            return false;
        }
        return emit_json(webview_id, event, data);
    }

    bool emit_all(std::string_view event) { return emit_json(kAllWebViews, event, "null"); }

    /// @brief Posts an already-serialized JSON payload. Any thread.
    bool emit_json(std::uint64_t webview_id, std::string_view event, std::string_view data_json);

    /// @brief true when source_uri's origin is in allowed (case-insensitive; "*" matches anything).
    [[nodiscard]] static bool is_origin_allowed(std::string_view source_uri, std::span<const std::string> allowed);
    /// @brief "scheme://host[:port]" of uri, lower-cased; empty for opaque or malformed URIs.
    [[nodiscard]] static std::string origin_of(std::string_view uri);

private:
    struct Attachment {
        core::WebView* webview = nullptr;
        std::uint64_t id = 0;
        std::vector<std::string> allowed_origins;
    };

    void on_web_message(std::uint64_t webview_id, std::string_view raw_json, std::string_view source_uri);
    void deliver(std::uint64_t webview_id, std::string_view json) const;
    [[nodiscard]] const Attachment* find(std::uint64_t webview_id) const noexcept;

    exec::TaskRunner& m_runner;
    exec::UiMarshal& m_marshal;
    DispatchCallback m_dispatch_fn;
    std::vector<Attachment> m_attachments; // UI thread only
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // UI thread only
};

}  // namespace boson::ipc
