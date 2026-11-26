#pragma once

#include <windows.h>
#include <oaidl.h>
#include <wrl.h>
#include <WebView2.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "boson/core/webview.hpp"
#include "boson/ipc/export.hpp"
#include "boson/ipc/router.hpp"
#include "boson/security/capabilities.hpp"

namespace boson::ipc {

/// JSON-RPC error code for a method that exists but is not enabled for the synchronous path.
inline constexpr int kSyncNotAllowedCode = -32005;
/// Default name of the host object: chrome.webview.hostObjects.sync.boson.invoke(method, paramsJson).
inline constexpr std::wstring_view kSyncHostObjectName = L"boson";

/// @brief A controller that opts some (or all) of its methods into SyncBridge:
///   static constexpr std::array<std::string_view, 2> boson_sync_methods() { return {"get", "peek"}; }
/// A single "*" enables every method. Sync handlers run on the UI thread (see docs/SYNC_IPC.md).
template <typename T>
concept SyncController = BosonController<T> && requires {
    { T::boson_sync_methods() };
};

class SyncBridge;

struct ZoomFactorResponse {
    double value = 1.0;
};

struct ZoomFactorRequest {
    double value = 1.0;
};

struct FilePathsRequest {
    std::uint64_t token = 0;
};

struct FilePathsResponse {
    std::vector<std::string> paths;
};

/// @brief JS "SyncHost.*": per-WebView values that only make sense synchronously (Electron's webFrame zoom and
/// webUtils.getPathForFile). Always served by the SyncBridge itself, on the UI thread, for the calling WebView;
/// calling it through the asynchronous Router path throws (the WebView must not be touched off the UI thread).
class SyncHostController {
public:
    BOSON_CONTROLLER("SyncHost");

    static constexpr std::array<std::string_view, 1> boson_sync_methods() noexcept { return {"*"}; }

    explicit SyncHostController(SyncBridge* bridge = nullptr) noexcept : m_bridge(bridge) {}

    /// @brief ICoreWebView2Controller::get_ZoomFactor of the calling WebView.
    [[nodiscard]] ZoomFactorResponse zoom_factor() const;
    /// @brief put_ZoomFactor (0.25 .. 5.0).
    void set_zoom_factor(ZoomFactorRequest req) const;
    /// @brief Real paths of the File objects the page posted with
    /// postMessageWithAdditionalObjects({__boson_sync:'files', token}, files) just before this call.
    [[nodiscard]] FilePathsResponse file_paths(FilePathsRequest req) const;

private:
    [[nodiscard]] core::WebView& sender_webview() const;

    SyncBridge* m_bridge;
};

struct SyncAttachOptions {
    /// Origins (scheme://host[:port]) whose top-level document may call the host object. Empty = boson://app plus
    /// the origin of the WebView's dev_url. "*" allows every origin (never for third-party content).
    std::vector<std::string> allowed_origins;
    /// Checked for every sync call, exactly as Bridge does for async calls (null = no method-level checks).
    std::shared_ptr<const security::CapabilityPolicy> capabilities;
    /// Host object name (JS: chrome.webview.hostObjects.sync.<name>). boson.invokeSync() expects "boson".
    std::wstring object_name{kSyncHostObjectName};
};

/// @brief Counters of the synchronous path (UI thread; read them on the UI thread).
struct SyncStats {
    std::uint64_t calls = 0;          ///< calls that reached a handler
    std::uint64_t rejected = 0;       ///< origin / allow-list / capability rejections
    std::uint64_t over_budget = 0;    ///< handler calls slower than budget()
    std::uint64_t total_ns = 0;       ///< time spent in handlers (UI thread time)
    std::uint64_t max_ns = 0;
};

/// @brief Synchronous JS -> C++ calls (Electron's ipcRenderer.sendSync) through a WebView2 host object.
///
/// attach() adds an IDispatch host object with AddHostObjectToScript; the page calls
/// chrome.webview.hostObjects.sync.boson.invoke(method, paramsJson) (frontend: boson.invokeSync()). The call
/// blocks the page's JS thread in the renderer process until the handler returns. WebView2 delivers it to the
/// UI thread that owns the WebView, so the handler runs ON THE UI THREAD: it must be cheap (microseconds), must
/// not wait for the UI thread (UiMarshal::invoke, WebViewController) and must not wait for the page (deadlock).
/// Only methods in the sync allow-list are reachable (SyncController::boson_sync_methods() plus explicit
/// patterns), after the same origin check and CapabilityPolicy as Bridge. Handler exceptions become error
/// envelopes; nothing is thrown across the COM boundary. For values known before the page loads, prefer
/// BootValues (boson/ipc/sync_boot.hpp): no round trip at all.
///
/// All members: UI thread only. A WebView must be detached (or the SyncBridge destroyed) before it is destroyed.
/// The host object belongs to the WebView2 controller: call attach() again after Environment::recreate.
class SyncBridge {
public:
    /// Receives a complete JSON-RPC request ({"id":0,"method":..,"params":..}); returns the response envelope.
    using DispatchFn = std::function<std::string(std::string_view request_json)>;

    /// @param sync_methods method patterns callable synchronously ("Controller.method", "Controller.*").
    SyncBridge(DispatchFn dispatch, std::vector<std::string> sync_methods);

    /// @brief Dispatches through router (Router::dispatch_sync on the UI thread). Methods of SyncController
    /// controllers are allowed automatically; extra_sync_methods adds patterns for the others.
    template <BosonController... Controllers>
    explicit SyncBridge(Router<Controllers...>& router, std::vector<std::string> extra_sync_methods = {})
        : SyncBridge(
              [&router](std::string_view request_json) { return router.dispatch_sync(request_json); },
              collect_sync_methods<Controllers...>(std::move(extra_sync_methods))) {}

    SyncBridge(const SyncBridge&) = delete;
    SyncBridge& operator=(const SyncBridge&) = delete;
    SyncBridge(SyncBridge&&) = delete;
    SyncBridge& operator=(SyncBridge&&) = delete;
    ~SyncBridge();

    /// @brief Adds the host object to webview (must be ready). false when host objects are disabled
    /// (ICoreWebView2Settings::AreHostObjectsAllowed) or AddHostObjectToScript fails.
    bool attach(core::WebView& webview, SyncAttachOptions options = {});
    void detach(core::WebView& webview) noexcept;
    [[nodiscard]] bool is_attached(std::uint64_t webview_id) const noexcept;
    bool set_capabilities(std::uint64_t webview_id, std::shared_ptr<const security::CapabilityPolicy> policy) noexcept;
    [[nodiscard]] core::WebView* find_webview(std::uint64_t webview_id) const noexcept;

    /// @brief What the host object calls. Checks origin (the WebView's current top-level Source), the sync
    /// allow-list and the capability policy, then dispatches. Always returns a response envelope.
    [[nodiscard]] std::string invoke(std::uint64_t webview_id, std::string_view method, std::string_view params_json);
    /// @brief invoke() with an explicit source URI instead of the WebView's Source (tests, custom hosts).
    [[nodiscard]] std::string invoke_from(
        std::uint64_t webview_id, std::string_view source_uri, std::string_view method, std::string_view params_json);
    /// @brief The checks + dispatch behind every sync call, without an attached WebView (headless tests, custom
    /// hosts). sender_id becomes current_sender_id() for the handler. Never throws.
    [[nodiscard]] std::string invoke_checked(
        std::span<const std::string> allowed_origins,
        const security::CapabilityPolicy* capabilities,
        std::uint64_t sender_id,
        std::string_view source_uri,
        std::string_view method,
        std::string_view params_json) noexcept;

    [[nodiscard]] bool is_sync_method(std::string_view method) const noexcept;
    [[nodiscard]] const std::vector<std::string>& sync_methods() const noexcept { return m_sync_patterns; }

    /// @brief Handler time above which a call is counted (and logged) as over budget. Default 2 ms.
    void set_budget(std::chrono::nanoseconds budget) noexcept { m_budget = budget; }
    [[nodiscard]] std::chrono::nanoseconds budget() const noexcept { return m_budget; }
    [[nodiscard]] const SyncStats& stats() const noexcept { return m_stats; }

    /// @brief Paths posted by webview's page for token (see SyncHostController::file_paths); removed on read.
    [[nodiscard]] std::optional<std::vector<std::string>> take_file_paths(std::uint64_t webview_id, std::uint64_t token);

    using InvokeFn = std::function<std::string(std::string_view method, std::string_view params_json)>;
    static constexpr DISPID kInvokeDispId = 1;
    /// @brief The bare IDispatch: one method, invoke(method: string, params?: string) -> string. Exposed for tests
    /// and custom hosts; attach() uses it.
    [[nodiscard]] static Microsoft::WRL::ComPtr<IDispatch> make_host_object(InvokeFn fn);

    /// @brief true when method is a well-formed "Controller.method" made of [A-Za-z0-9_$:.-], at most 256 chars.
    [[nodiscard]] static bool is_valid_method_name(std::string_view method) noexcept;

private:
    struct Attachment {
        core::WebView* webview = nullptr;
        std::uint64_t id = 0;
        std::wstring object_name;
        std::vector<std::string> allowed_origins;
        std::shared_ptr<const security::CapabilityPolicy> capabilities;
        core::WebView::HookId files_hook = 0;
    };

    struct PendingFiles {
        std::uint64_t webview_id = 0;
        std::uint64_t token = 0;
        std::vector<std::string> paths;
    };

    template <BosonController... Controllers>
    static std::vector<std::string> collect_sync_methods(std::vector<std::string> extra) {
        (append_controller_patterns<Controllers>(extra), ...);
        return extra;
    }

    template <BosonController Controller>
    static void append_controller_patterns(std::vector<std::string>& out) {
        if constexpr (SyncController<Controller>) {
            for (const auto& name : Controller::boson_sync_methods()) {
                std::string pattern{Controller::boson_controller_name()};
                pattern.push_back('.');
                pattern.append(std::string_view{name});
                out.push_back(std::move(pattern));
            }
        }
    }

    [[nodiscard]] const Attachment* find(std::uint64_t webview_id) const noexcept;
    bool on_web_message(std::uint64_t webview_id, const core::WebView::WebMessage& message);
    [[nodiscard]] std::string dispatch(std::string_view method, std::string_view params_json);

    DispatchFn m_dispatch;
    std::vector<std::string> m_sync_patterns;
    std::shared_ptr<const security::CapabilityPolicy> m_sync_policy;
    SyncHostController m_host_controller;
    Router<SyncHostController> m_host_router;
    std::vector<Attachment> m_attachments;
    std::vector<PendingFiles> m_pending_files;
    std::chrono::nanoseconds m_budget = std::chrono::milliseconds{2};
    SyncStats m_stats;
    DWORD m_ui_thread = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    friend class SyncHostController;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::SyncHostController> {
    using T = boson::ipc::SyncHostController;
    static constexpr auto value = glz::object(
        "zoom_factor", &T::zoom_factor,
        "set_zoom_factor", &T::set_zoom_factor,
        "file_paths", &T::file_paths);
};
