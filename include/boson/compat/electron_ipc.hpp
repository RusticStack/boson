#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glaze/glaze.hpp>

#include "boson/ipc/bridge.hpp"
#include "boson/ipc/export.hpp"
#include "boson/ipc/request_context.hpp"

namespace boson::compat {

/// @brief Prefix of the Bridge event that carries an ElectronIpc channel message ("ipc:<channel>").
inline constexpr std::string_view kIpcEventPrefix = "ipc:";

/// @brief Bridge event name for an ElectronIpc channel: "ipc:" + channel.
[[nodiscard]] std::string ipc_event_name(std::string_view channel);

/// @brief Serializes each argument with glaze into a JSON array ("[a,b,...]"). Empty string on failure.
template <typename... Args>
[[nodiscard]] std::string encode_ipc_args(const Args&... args) {
    std::string out;
    out.push_back('[');
    bool ok = true;
    bool first = true;
    const auto append = [&](const auto& value) {
        if (!ok) {
            return;
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        std::string item;
        if (glz::write_json(value, item)) {
            ok = false;
            return;
        }
        out.append(item);
    };
    (append(args), ...);
    out.push_back(']');
    return ok ? out : std::string{};
}

class ElectronIpc;

/// @brief What a handler / listener learns about the message (Electron's IpcMainInvokeEvent / IpcMainEvent).
struct IpcEvent {
    /// WebView::id() of the sending WebView (0 when the request did not come through a Bridge).
    std::uint64_t sender_id = 0;
    std::string_view channel;
    ElectronIpc* ipc = nullptr;

    /// @brief event.reply / event.sender.send: posts "ipc:<channel>" with args to the sender only.
    template <typename... Args>
    bool reply(std::string_view reply_channel, const Args&... args) const;
};

/// @brief ipcMain-style named handlers and listeners plus webContents.send-style emitters, exposed to JS as the
/// dynamic controller "ElectronIpc" (methods invoke / send / emit). Add it to a Router next to the static
/// controllers; the Router still parses the envelope, runs everything on the TaskRunner pool, catches every
/// exception and replies to the sending WebView. Static controllers keep their zero-overhead reflected path.
///
/// Thread safety: every member may be called from any thread, including from inside a handler.
/// Handlers and listeners run on the pool thread that serves the request (never on the UI thread); use
/// exec::UiMarshal for UI-affine work.
class ElectronIpc {
public:
    BOSON_CONTROLLER("ElectronIpc");

    using Json = glz::generic;
    /// args is the JSON array of the arguments passed to ipcRenderer.invoke(channel, ...args).
    using Handler = std::function<Json(const IpcEvent& event, Json args)>;
    /// Zero-parse form: args_json is the raw JSON array text; the return value must be valid JSON text
    /// (empty = null). Invalid JSON is turned into an error reply instead of reaching the WebView.
    using RawHandler = std::function<std::string(const IpcEvent& event, std::string_view args_json)>;
    using Listener = std::function<void(const IpcEvent& event, const Json& args)>;
    using ListenerId = std::uint64_t;

    explicit ElectronIpc(ipc::Bridge* bridge = nullptr) noexcept : m_bridge(bridge) {}

    ElectronIpc(const ElectronIpc&) = delete;
    ElectronIpc& operator=(const ElectronIpc&) = delete;
    ElectronIpc(ElectronIpc&&) = delete;
    ElectronIpc& operator=(ElectronIpc&&) = delete;
    ~ElectronIpc() = default;

    /// @brief The Bridge used by send()/broadcast() and to re-broadcast frontend emit() calls.
    void set_bridge(ipc::Bridge* bridge) noexcept { m_bridge.store(bridge, std::memory_order_release); }
    [[nodiscard]] ipc::Bridge* bridge() const noexcept { return m_bridge.load(std::memory_order_acquire); }

    // ---- ipcMain.handle / handleOnce / removeHandler --------------------------------------------------------
    /// @brief Registers the invoke() handler for channel. false when the channel already has one (Electron throws).
    bool handle(std::string channel, Handler handler);
    bool handle_raw(std::string channel, RawHandler handler);
    /// @brief Like handle(), but the handler is removed before its first invocation runs.
    bool handle_once(std::string channel, Handler handler);
    bool remove_handler(std::string_view channel);
    [[nodiscard]] bool has_handler(std::string_view channel) const;

    // ---- ipcMain.on / once / removeListener / removeAllListeners ------------------------------------------
    /// @brief Listener for ipcRenderer.send(channel, ...args).
    ListenerId on(std::string channel, Listener listener);
    ListenerId once(std::string channel, Listener listener);
    /// @brief Listener for frontend-emitted events (Tauri emit(), Wails Events.Emit()); args = [payload].
    ListenerId on_event(std::string event, Listener listener);
    bool remove_listener(ListenerId id);
    /// @brief Removes the listeners (both kinds) named channel; an empty channel removes every listener.
    void remove_all_listeners(std::string_view channel = {});
    [[nodiscard]] std::size_t listener_count(std::string_view channel) const;

    // ---- webContents.send ----------------------------------------------------------------------------------
    /// @brief Posts "ipc:<channel>" with the glaze-serialized args to one WebView (WebView::id()).
    template <typename... Args>
    bool send(std::uint64_t webview_id, std::string_view channel, const Args&... args) {
        const std::string array = encode_ipc_args(args...);
        return !array.empty() && send_json(webview_id, channel, array);
    }
    /// @brief send() to every WebView attached to the Bridge.
    template <typename... Args>
    bool broadcast(std::string_view channel, const Args&... args) {
        return send(ipc::Bridge::kAllWebViews, channel, args...);
    }
    /// @brief Posts an already-encoded JSON array of arguments. false without a Bridge.
    bool send_json(std::uint64_t webview_id, std::string_view channel, std::string_view args_array_json);

    // ---- Router integration (DynamicController) --------------------------------------------------------
    /// @brief Serves "ElectronIpc.invoke" {channel,args}, "ElectronIpc.send" {channel,args} and
    /// "ElectronIpc.emit" {event,payload,broadcast}. Returns false for any other method name.
    bool boson_dispatch(
        std::string_view method,
        std::uint64_t request_id,
        std::string_view params_json,
        std::string& out_response) noexcept;

private:
    enum class ListenerKind : std::uint8_t { Channel, Event };

    struct HandlerEntry {
        Handler handler;
        RawHandler raw;
        bool once = false;
    };

    struct ListenerEntry {
        ListenerId id = 0;
        ListenerKind kind = ListenerKind::Channel;
        std::string name;
        bool once = false;
        std::shared_ptr<const Listener> listener;
    };

    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    bool add_handler(std::string channel, std::shared_ptr<const HandlerEntry> entry);
    ListenerId add_listener(ListenerKind kind, std::string name, Listener listener, bool once);
    std::string invoke(std::uint64_t request_id, std::string_view params_json);
    std::string send_from_renderer(std::uint64_t request_id, std::string_view params_json);
    std::string emit_from_renderer(std::uint64_t request_id, std::string_view params_json);
    void notify_listeners(ListenerKind kind, std::string_view name, std::string_view args_json);

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const HandlerEntry>, StringHash, std::equal_to<>> m_handlers;
    std::vector<ListenerEntry> m_listeners;
    ListenerId m_next_listener_id = 1;
    std::atomic<ipc::Bridge*> m_bridge;
};

template <typename... Args>
bool IpcEvent::reply(std::string_view reply_channel, const Args&... args) const {
    // sender_id 0 is "unknown", not Bridge::kAllWebViews: never turn a reply into a broadcast.
    return ipc != nullptr && sender_id != 0 && ipc->send(sender_id, reply_channel, args...);
}

} // namespace boson::compat
