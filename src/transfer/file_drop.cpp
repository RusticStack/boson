#include <boson/transfer/file_drop.hpp>

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>

#include "transfer_internal.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace boson::transfer {

namespace {

using Microsoft::WRL::ComPtr;

// Reports files dropped anywhere in the page. Drop zones created by transfer.ts call preventDefault() in their
// own (earlier, bubbling) drop listener, so this window-level listener skips events they already handled.
constexpr std::wstring_view kPageScript = LR"JS((() => {
  if (window.__bosonFileDrop) return;
  window.__bosonFileDrop = true;
  const wv = window.chrome && window.chrome.webview;
  if (!wv || typeof wv.postMessageWithAdditionalObjects !== 'function') return;
  const hasFiles = (e) => !!e.dataTransfer && Array.prototype.indexOf.call(e.dataTransfer.types || [], 'Files') >= 0;
  window.addEventListener('dragover', (e) => {
    if (hasFiles(e)) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; }
  });
  window.addEventListener('drop', (e) => {
    if (!hasFiles(e)) return;
    const handled = e.defaultPrevented;
    e.preventDefault();
    if (handled) return;
    const files = Array.from(e.dataTransfer.files || []);
    if (files.length === 0) return;
    wv.postMessageWithAdditionalObjects({ __boson_transfer: 'drop', zone: '', x: e.clientX, y: e.clientY, meta: null }, files);
  });
})();)JS";

} // namespace

FileDrop::FileDrop(core::Window& window, Callback callback, bool accept_window_drops)
    : m_window(window),
      m_callback(std::move(callback)) {
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) {
        return on_window_message(msg, w_param, l_param);
    });
    set_accept_window_drops(accept_window_drops);
}

FileDrop::~FileDrop() {
    *m_alive = false;
    for (const auto& attachment : m_attachments) {
        attachment.webview->remove_web_message_observer(attachment.hook);
    }
    m_attachments.clear();
    if (m_hook != 0) {
        m_window.remove_message_hook(m_hook);
    }
    if (m_accept_window && m_window.hwnd() != nullptr) {
        DragAcceptFiles(m_window.hwnd(), FALSE);
    }
}

void FileDrop::set_accept_window_drops(bool accept) {
    m_accept_window = accept;
    const HWND hwnd = m_window.hwnd();
    if (hwnd == nullptr) {
        return;
    }
    DragAcceptFiles(hwnd, accept ? TRUE : FALSE);
    if (accept) {
        // Let a non-elevated Explorer drop onto an elevated app (UIPI blocks these messages otherwise).
        constexpr UINT kWmCopyGlobalData = 0x0049;
        for (const UINT msg : {static_cast<UINT>(WM_DROPFILES), static_cast<UINT>(WM_COPYDATA), kWmCopyGlobalData}) {
            if (ChangeWindowMessageFilterEx(hwnd, msg, MSGFLT_ALLOW, nullptr) == FALSE) {
                core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "FileDrop: ChangeWindowMessageFilterEx");
            }
        }
    }
}

std::wstring FileDrop::page_script() {
    return std::wstring(kPageScript);
}

std::vector<std::string> FileDrop::paths_from_hdrop(HDROP drop) {
    std::vector<std::string> out;
    if (drop == nullptr) {
        return out;
    }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
    out.reserve(count);
    std::wstring buffer;
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) {
            continue;
        }
        buffer.assign(static_cast<std::size_t>(length) + 1, L'\0');
        const UINT copied = DragQueryFileW(drop, i, buffer.data(), length + 1);
        if (copied == 0) {
            continue;
        }
        out.push_back(core::wide_to_utf8(std::wstring_view(buffer.data(), copied)));
    }
    return out;
}

std::vector<std::string> FileDrop::paths_from_objects(ICoreWebView2ObjectCollectionView* objects) {
    std::vector<std::string> out;
    if (objects == nullptr) {
        return out;
    }
    UINT32 count = 0;
    if (!BOSON_HR_CHECK(objects->get_Count(&count))) {
        return out;
    }
    out.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IUnknown> item;
        if (!BOSON_HR_CHECK(objects->GetValueAtIndex(i, &item)) || !item) {
            continue;
        }
        ComPtr<ICoreWebView2File> file;
        if (FAILED(item.As(&file))) {
            continue; // FileSystemHandle or another object kind: no path to report
        }
        wchar_t* raw = nullptr;
        const HRESULT hr = file->get_Path(&raw);
        if (core::hr_check(hr, "ICoreWebView2File::get_Path") && raw != nullptr && raw[0] != L'\0') {
            out.push_back(core::wide_to_utf8(raw));
        }
        if (raw != nullptr) {
            CoTaskMemFree(raw);
        }
    }
    return out;
}

std::optional<FileDrop::DropMessage> FileDrop::parse_drop_message(std::string_view json) {
    detail::WireMessage wire;
    if (!detail::parse_wire(json, wire) || wire.op != "drop") {
        return std::nullopt;
    }
    if (!std::isfinite(wire.x) || !std::isfinite(wire.y)) {
        wire.x = 0.0;
        wire.y = 0.0;
    }
    return DropMessage{
        .zone = std::move(wire.zone),
        .x = wire.x,
        .y = wire.y,
        .metadata_json = detail::raw_or_null(wire.meta),
    };
}

POINT FileDrop::css_to_client(const core::WebView& webview, double css_x, double css_y) noexcept {
    const HWND parent = webview.parent_hwnd();
    const UINT dpi = parent != nullptr ? GetDpiForWindow(parent) : USER_DEFAULT_SCREEN_DPI;
    const double scale = static_cast<double>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi) / USER_DEFAULT_SCREEN_DPI *
                         webview.zoom_factor().value_or(1.0);
    const RECT origin = webview.bounds();
    return POINT{
        origin.left + static_cast<LONG>(std::lround(css_x * scale)),
        origin.top + static_cast<LONG>(std::lround(css_y * scale)),
    };
}

void FileDrop::attach(core::WebView& webview, FileDropAttachOptions options) {
    detach(webview);
    const std::uint64_t id = webview.id();
    Attachment attachment{
        .webview = &webview,
        .id = id,
        .hook = 0,
        .allowed_origins = detail::default_origins(webview, std::move(options.allowed_origins)),
    };
    attachment.hook = webview.add_web_message_observer([this, alive = m_alive, id](const core::WebView::WebMessage& message) {
        return *alive && on_web_message(id, message);
    });
    m_attachments.push_back(std::move(attachment));

    if (options.allow_external_drop.has_value() && webview.controller() != nullptr) {
        ComPtr<ICoreWebView2Controller4> controller4;
        if (SUCCEEDED(webview.controller()->QueryInterface(IID_PPV_ARGS(&controller4)))) {
            BOSON_HR_CHECK(controller4->put_AllowExternalDrop(*options.allow_external_drop ? TRUE : FALSE));
        }
    }

    if (options.inject_page_handler && webview.is_ready()) {
        const std::string script = core::wide_to_utf8(kPageScript);
        webview.add_script_on_document_created(script, [](HRESULT hr, std::string) {
            core::hr_check(hr, "FileDrop: AddScriptToExecuteOnDocumentCreated");
        });
        webview.execute_script(script, [](HRESULT hr, std::string) { core::hr_check(hr, "FileDrop: ExecuteScript"); });
    }
}

void FileDrop::detach(core::WebView& webview) noexcept {
    const auto it = std::ranges::find(m_attachments, &webview, &Attachment::webview);
    if (it != m_attachments.end()) {
        webview.remove_web_message_observer(it->hook);
        m_attachments.erase(it);
    }
}

bool FileDrop::is_attached(std::uint64_t webview_id) const noexcept {
    return std::ranges::find(m_attachments, webview_id, &Attachment::id) != m_attachments.end();
}

bool FileDrop::on_web_message(std::uint64_t webview_id, const core::WebView::WebMessage& message) {
    if (!detail::is_transfer_message(message.json)) {
        return false;
    }
    auto drop = parse_drop_message(message.json);
    if (!drop.has_value()) {
        return false; // another transfer message (BinaryTransport handles those)
    }
    const auto it = std::ranges::find(m_attachments, webview_id, &Attachment::id);
    if (it == m_attachments.end()) {
        return false;
    }
    if (!ipc::Bridge::is_origin_allowed(message.source_uri, it->allowed_origins)) {
        core::hr_check(E_ACCESSDENIED, "FileDrop: dropped file message from untrusted origin");
        return true;
    }

    FileDropEvent event;
    event.target = DropTarget::WebView;
    event.webview_id = webview_id;
    event.css_x = drop->x;
    event.css_y = drop->y;
    event.client_px = css_to_client(*it->webview, drop->x, drop->y);
    event.zone = std::move(drop->zone);
    event.metadata_json = std::move(drop->metadata_json);
    event.source_uri = std::string(message.source_uri);

    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
    if (message.args != nullptr && SUCCEEDED(message.args->QueryInterface(IID_PPV_ARGS(&args2)))) {
        ComPtr<ICoreWebView2ObjectCollectionView> objects;
        if (BOSON_HR_CHECK(args2->get_AdditionalObjects(&objects)) && objects) {
            UINT32 count = 0;
            BOSON_HR_CHECK(objects->get_Count(&count));
            event.paths = paths_from_objects(objects.Get());
            event.unresolved_objects = count > event.paths.size() ? count - event.paths.size() : 0;
        }
    }
    if (!event.paths.empty() || event.unresolved_objects != 0) {
        deliver(event);
    }
    return true;
}

std::optional<LRESULT> FileDrop::on_window_message(UINT msg, WPARAM w_param, LPARAM) {
    if (msg != WM_DROPFILES) {
        return std::nullopt;
    }
    const auto drop = reinterpret_cast<HDROP>(w_param);
    FileDropEvent event;
    event.target = DropTarget::Window;
    event.paths = paths_from_hdrop(drop);
    POINT pt{};
    DragQueryPoint(drop, &pt); // FALSE only means "dropped on the non-client area"; pt is filled either way
    event.client_px = pt;
    DragFinish(drop);
    if (m_accept_window && !event.paths.empty()) {
        deliver(event);
    }
    return LRESULT{0};
}

void FileDrop::deliver(const FileDropEvent& event) const {
    if (!m_callback) {
        return;
    }
    try {
        m_callback(event);
    } catch (...) {
        core::hr_check(E_UNEXPECTED, "FileDrop: callback threw");
    }
}

} // namespace boson::transfer
