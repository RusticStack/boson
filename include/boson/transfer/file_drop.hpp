#pragma once

#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/core/window.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::transfer {

enum class DropTarget {
    Window,  ///< WM_DROPFILES on a part of the window that no WebView covers
    WebView, ///< A drop inside a page (chrome.webview.postMessageWithAdditionalObjects)
};

struct FileDropEvent {
    DropTarget target = DropTarget::Window;
    /// Absolute UTF-8 paths, in the order the shell / the page reported them.
    std::vector<std::string> paths;
    /// Objects the page attached that carry no file-system path (e.g. a File built in script). WebView drops only.
    std::size_t unresolved_objects = 0;
    /// Drop position in the window's client area, physical pixels.
    POINT client_px{};
    /// Drop position in CSS pixels of the page (WebView drops only).
    double css_x = 0.0;
    double css_y = 0.0;
    /// WebView::id() of the WebView that received the drop; 0 for window drops.
    std::uint64_t webview_id = 0;
    /// Drop-zone name from transfer.ts createDropZone(); empty for the page-wide handler.
    std::string zone;
    /// JSON the page attached to the drop ("null" when none).
    std::string metadata_json = "null";
    /// Page URL that sent the drop (WebView drops only).
    std::string source_uri;
};

struct FileDropAttachOptions {
    /// Origins allowed to report drops. Empty = boson://app plus the origin of the WebView's dev_url.
    std::vector<std::string> allowed_origins;
    /// Adds FileDrop::page_script() as a document-created script (and runs it in the current document), so
    /// files dropped anywhere in the page are reported even without transfer.ts.
    bool inject_page_handler = true;
    /// ICoreWebView2Controller4::put_AllowExternalDrop. Unset keeps the runtime default (allowed).
    std::optional<bool> allow_external_drop;
};

/// @brief Native file drag-and-drop with real paths, for one Window and any number of its WebViews.
/// UI thread only. The callback runs on the UI thread; offload heavy work (e.g. TaskRunner::spawn).
class FileDrop {
public:
    using Callback = std::function<void(const FileDropEvent& event)>;

    /// @param accept_window_drops calls DragAcceptFiles on the window so drops outside WebViews arrive too.
    explicit FileDrop(core::Window& window, Callback callback = {}, bool accept_window_drops = true);
    ~FileDrop();

    FileDrop(const FileDrop&) = delete;
    FileDrop& operator=(const FileDrop&) = delete;
    FileDrop(FileDrop&&) = delete;
    FileDrop& operator=(FileDrop&&) = delete;

    void set_callback(Callback callback) { m_callback = std::move(callback); }
    void set_accept_window_drops(bool accept);
    [[nodiscard]] bool accepts_window_drops() const noexcept { return m_accept_window; }

    /// @brief Starts receiving drops reported by webview's page. The WebView must outlive this FileDrop or be
    /// detached first. Call after the WebView is ready to have the page handler injected immediately; to keep
    /// it across Environment::recreate, also put page_script() in WebViewConfig::scripts_on_document_created.
    void attach(core::WebView& webview, FileDropAttachOptions options = {});
    void detach(core::WebView& webview) noexcept;
    [[nodiscard]] bool is_attached(std::uint64_t webview_id) const noexcept;

    /// @brief Script that reports files dropped anywhere in a page (idempotent; zones from transfer.ts win).
    [[nodiscard]] static std::wstring page_script();

    /// @brief Absolute paths stored in an HDROP (DragQueryFileW), UTF-8.
    [[nodiscard]] static std::vector<std::string> paths_from_hdrop(HDROP drop);

    /// @brief Paths of the ICoreWebView2File objects in an additional-objects collection; other objects skipped.
    [[nodiscard]] static std::vector<std::string> paths_from_objects(ICoreWebView2ObjectCollectionView* objects);

    struct DropMessage {
        std::string zone;
        double x = 0.0;
        double y = 0.0;
        std::string metadata_json = "null";
    };
    /// @brief Parses {"__boson_transfer":"drop",...}; std::nullopt for any other message.
    [[nodiscard]] static std::optional<DropMessage> parse_drop_message(std::string_view json);

    /// @brief Maps a CSS-pixel point of a WebView to window client pixels (DPI, zoom and WebView bounds).
    [[nodiscard]] static POINT css_to_client(const core::WebView& webview, double css_x, double css_y) noexcept;

private:
    struct Attachment {
        core::WebView* webview = nullptr;
        std::uint64_t id = 0;
        core::WebView::HookId hook = 0;
        std::vector<std::string> allowed_origins;
    };

    std::optional<LRESULT> on_window_message(UINT msg, WPARAM w_param, LPARAM l_param);
    bool on_web_message(std::uint64_t webview_id, const core::WebView::WebMessage& message);
    void deliver(const FileDropEvent& event) const;

    core::Window& m_window;
    Callback m_callback;
    core::Window::ListenerId m_hook = 0;
    bool m_accept_window = false;
    std::vector<Attachment> m_attachments;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace boson::transfer
