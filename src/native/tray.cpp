#include <boson/native/tray.hpp>

#include <boson/core/result.hpp>
#include <boson/native/notification.hpp>

#include <objbase.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace boson::native {

namespace {

constexpr wchar_t kTrayWindowClass[] = L"BosonTrayWindow";

void copy_truncated(wchar_t* dst, std::size_t cap, std::wstring_view src) noexcept {
    if (cap == 0) {
        return;
    }
    std::size_t count = (std::min)(src.size(), cap - 1);
    // Do not split a surrogate pair.
    if (count > 0 && count < src.size() && src[count - 1] >= 0xD800 && src[count - 1] <= 0xDBFF) {
        --count;
    }
    std::wmemcpy(dst, src.data(), count);
    dst[count] = L'\0';
}

bool register_tray_class(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = kTrayWindowClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassExW(BosonTrayWindow)");
        return false;
    }
    return true;
}

/// Balances CoInitializeEx when this call was the one that initialized COM on the thread.
class ComScope {
public:
    ComScope() noexcept : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT m_hr;
};

struct GdiBitmap {
    HBITMAP handle = nullptr;
    ~GdiBitmap() {
        if (handle != nullptr) {
            DeleteObject(handle);
        }
    }
};

} // namespace

void IconDeleter::operator()(HICON icon) const noexcept {
    if (icon != nullptr) {
        DestroyIcon(icon);
    }
}

int small_icon_size(UINT dpi) noexcept {
    const UINT effective = dpi != 0 ? dpi : GetDpiForSystem();
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, effective);
    return size > 0 ? size : 16;
}

UniqueIcon load_icon_from_image(std::span<const std::byte> bytes, int size_px) {
    using Microsoft::WRL::ComPtr;
    if (bytes.empty() || size_px <= 0 || size_px > 1024 || bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        return nullptr;
    }
    const ComScope com;

    ComPtr<IWICImagingFactory> factory;
    if (!BOSON_HR_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    ComPtr<IWICStream> stream;
    if (!BOSON_HR_CHECK(factory->CreateStream(&stream))) {
        return nullptr;
    }
    // WIC only reads from the buffer; the API is not const-correct.
    auto* data = reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data()));
    if (!BOSON_HR_CHECK(stream->InitializeFromMemory(data, static_cast<DWORD>(bytes.size())))) {
        return nullptr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        return nullptr; // not an image format WIC knows
    }

    UINT frame_count = 0;
    if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count == 0) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> best;
    UINT best_width = 0;
    for (UINT i = 0; i < frame_count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0;
        UINT h = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
            continue;
        }
        const auto wanted = static_cast<UINT>(size_px);
        const bool better = best == nullptr ||
                            (w >= wanted && (best_width < wanted || w < best_width)) ||
                            (w < wanted && best_width < wanted && w > best_width);
        if (better) {
            best = frame;
            best_width = w;
        }
    }
    if (best == nullptr) {
        return nullptr;
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (!BOSON_HR_CHECK(factory->CreateBitmapScaler(&scaler)) ||
        !BOSON_HR_CHECK(scaler->Initialize(best.Get(), static_cast<UINT>(size_px), static_cast<UINT>(size_px), WICBitmapInterpolationModeFant))) {
        return nullptr;
    }
    ComPtr<IWICFormatConverter> converter;
    if (!BOSON_HR_CHECK(factory->CreateFormatConverter(&converter)) ||
        !BOSON_HR_CHECK(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size_px;
    info.bmiHeader.biHeight = -size_px; // top-down, matches WIC row order
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    GdiBitmap color;
    color.handle = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (color.handle == nullptr || bits == nullptr) {
        return nullptr;
    }
    const UINT stride = static_cast<UINT>(size_px) * 4;
    if (!BOSON_HR_CHECK(converter->CopyPixels(nullptr, stride, stride * static_cast<UINT>(size_px), static_cast<BYTE*>(bits)))) {
        return nullptr;
    }
    GdiFlush();

    const std::size_t mask_stride = ((static_cast<std::size_t>(size_px) + 15) / 16) * 2;
    const std::vector<BYTE> mask_bits(mask_stride * static_cast<std::size_t>(size_px), 0);
    GdiBitmap mask;
    mask.handle = CreateBitmap(size_px, size_px, 1, 1, mask_bits.data());
    if (mask.handle == nullptr) {
        return nullptr;
    }

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmMask = mask.handle;
    icon_info.hbmColor = color.handle;
    return UniqueIcon(CreateIconIndirect(&icon_info));
}

std::string_view tray_event_name(TrayEventKind kind) noexcept {
    switch (kind) {
    case TrayEventKind::LeftClick:
        return "left";
    case TrayEventKind::RightClick:
        return "right";
    case TrayEventKind::DoubleClick:
        return "double";
    case TrayEventKind::MiddleClick:
        return "middle";
    case TrayEventKind::KeyboardSelect:
        return "keyboard";
    case TrayEventKind::BalloonClicked:
        return "balloon-click";
    case TrayEventKind::BalloonDismissed:
        return "balloon-dismissed";
    }
    return "left";
}

UINT TrayIcon::taskbar_created_message() noexcept {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

TrayIcon::TrayIcon(HWND owner)
    : m_owner(owner)
    , m_menu({}, 1) {
    m_roles.on_quit = {};
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (register_tray_class(instance, &TrayIcon::wnd_proc)) {
        // A hidden top-level (not message-only) window: only those receive the TaskbarCreated broadcast.
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClass, L"BosonTray", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, instance, this);
        if (m_hwnd == nullptr) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(BosonTrayWindow)");
        } else {
            // Let an elevated process still hear Explorer (medium integrity) restarting.
            ChangeWindowMessageFilterEx(m_hwnd, taskbar_created_message(), MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(m_hwnd, kWmTrayCallback, MSGFLT_ALLOW, nullptr);
        }
    }

    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        HICON small_icon = nullptr;
        if (ExtractIconExW(exe, 0, nullptr, &small_icon, 1) > 0 && small_icon != nullptr) {
            m_default_icon.reset(small_icon);
        }
    }

    m_menu.set_listener([this](const menu::MenuItem& item, const menu::MenuClick& click) {
        if (m_menu_listener) {
            const auto listener = m_menu_listener;
            listener(item, click);
        }
    });
    set_owner(owner);
}

TrayIcon::~TrayIcon() {
    hide();
    if (m_hwnd != nullptr) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void TrayIcon::set_owner(HWND owner) noexcept {
    m_owner = owner;
}

void TrayIcon::set_role_context(menu::RoleContext context) {
    m_roles = std::move(context);
}

LRESULT CALLBACK TrayIcon::wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && msg != WM_NCCREATE) {
        return self->handle_message(msg, w_param, l_param);
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

LRESULT TrayIcon::handle_message(UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == kWmTrayCallback) {
        handle_callback(w_param, l_param);
        return 0;
    }
    if (msg == taskbar_created_message()) {
        // Explorer restarted: every notification icon is gone and must be added again.
        if (m_visible) {
            m_visible = false;
            reload_icon_for_dpi();
            add();
        }
        return 0;
    }
    if (msg == WM_DPICHANGED || (msg == WM_SETTINGCHANGE && w_param == SPI_SETICONMETRICS)) {
        reload_icon_for_dpi();
        if (m_visible) {
            modify(NIF_ICON);
        }
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, w_param, l_param);
}

void TrayIcon::handle_callback(WPARAM w_param, LPARAM l_param) {
    // NOTIFYICON_VERSION_4: LOWORD(lParam) = event, HIWORD(lParam) = icon id, wParam = anchor x/y.
    if (HIWORD(l_param) != m_id) {
        return;
    }
    const POINT anchor{GET_X_LPARAM(w_param), GET_Y_LPARAM(w_param)};
    switch (LOWORD(l_param)) {
    case NIN_SELECT:
        emit(TrayEventKind::LeftClick, anchor);
        if (m_menu_on_left_click && !m_menu.items().empty()) {
            popup_menu(anchor);
        }
        break;
    case NIN_KEYSELECT:
        emit(TrayEventKind::KeyboardSelect, anchor);
        break;
    case WM_CONTEXTMENU:
        emit(TrayEventKind::RightClick, anchor);
        if (!m_menu.items().empty()) {
            popup_menu(anchor);
        }
        break;
    case WM_LBUTTONDBLCLK:
        emit(TrayEventKind::DoubleClick, anchor);
        break;
    case WM_MBUTTONUP:
        emit(TrayEventKind::MiddleClick, anchor);
        break;
    case NIN_BALLOONUSERCLICK:
        emit(TrayEventKind::BalloonClicked, anchor);
        break;
    case NIN_BALLOONTIMEOUT:
    case NIN_BALLOONHIDE:
        emit(TrayEventKind::BalloonDismissed, anchor);
        break;
    default:
        break;
    }
}

void TrayIcon::emit(TrayEventKind kind, POINT position) {
    if (m_on_event) {
        const auto callback = m_on_event;
        callback(TrayEvent{.kind = kind, .position = position});
    }
}

bool TrayIcon::notify(DWORD message, NOTIFYICONDATAW& data) const {
    if (m_shell) {
        return m_shell(message, &data) != FALSE;
    }
    return Shell_NotifyIconW(message, &data) != FALSE;
}

NOTIFYICONDATAW TrayIcon::base_data() const noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(NOTIFYICONDATAW);
    data.hWnd = m_hwnd;
    data.uID = m_id;
    return data;
}

HICON TrayIcon::current_icon() const noexcept {
    if (m_icon != nullptr) {
        return m_icon.get();
    }
    if (m_default_icon != nullptr) {
        return m_default_icon.get();
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

HICON TrayIcon::icon() const noexcept {
    return current_icon();
}

bool TrayIcon::add() {
    if (m_hwnd == nullptr) {
        return false;
    }
    NOTIFYICONDATAW data = base_data();
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    data.uCallbackMessage = kWmTrayCallback;
    data.hIcon = current_icon();
    copy_truncated(data.szTip, std::size(data.szTip), core::utf8_to_wide(m_tooltip));
    if (!notify(NIM_ADD, data)) {
        // A stale icon with our id may survive a crash of a previous TrayIcon on this HWND: replace it.
        NOTIFYICONDATAW stale = base_data();
        notify(NIM_DELETE, stale);
        if (!notify(NIM_ADD, data)) {
            return false;
        }
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!notify(NIM_SETVERSION, data)) {
        core::hr_check(E_FAIL, "Shell_NotifyIconW(NIM_SETVERSION)");
    }
    m_visible = true;
    ++m_registrations;
    return true;
}

bool TrayIcon::modify(UINT flags) {
    if (!m_visible) {
        return true;
    }
    NOTIFYICONDATAW data = base_data();
    data.uFlags = flags | NIF_SHOWTIP;
    data.hIcon = current_icon();
    copy_truncated(data.szTip, std::size(data.szTip), core::utf8_to_wide(m_tooltip));
    return notify(NIM_MODIFY, data);
}

bool TrayIcon::show() {
    if (m_visible) {
        return true;
    }
    return add();
}

bool TrayIcon::show(HWND owner, std::string_view tooltip) {
    set_owner(owner);
    m_tooltip = std::string(tooltip);
    if (m_visible) {
        return modify(NIF_TIP);
    }
    return show();
}

void TrayIcon::hide() noexcept {
    if (!m_visible) {
        return;
    }
    NOTIFYICONDATAW data = base_data();
    notify(NIM_DELETE, data);
    m_visible = false;
}

bool TrayIcon::set_icon(HICON icon) {
    if (icon == nullptr) {
        m_icon.reset();
        m_icon_bytes.clear();
        return modify(NIF_ICON);
    }
    HICON copy = CopyIcon(icon);
    if (copy == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CopyIcon");
        return false;
    }
    m_icon.reset(copy);
    m_icon_bytes.clear();
    return modify(NIF_ICON);
}

bool TrayIcon::set_icon_from_bytes(std::span<const std::byte> bytes) {
    UniqueIcon decoded = load_icon_from_image(bytes, small_icon_size());
    if (decoded == nullptr) {
        return false;
    }
    m_icon = std::move(decoded);
    m_icon_bytes.assign(bytes.begin(), bytes.end());
    return modify(NIF_ICON);
}

void TrayIcon::reload_icon_for_dpi() {
    if (m_icon_bytes.empty()) {
        return;
    }
    if (UniqueIcon decoded = load_icon_from_image(m_icon_bytes, small_icon_size()); decoded != nullptr) {
        m_icon = std::move(decoded);
    }
}

bool TrayIcon::set_tooltip(std::string_view tooltip) {
    m_tooltip = std::string(tooltip);
    return modify(NIF_TIP);
}

void TrayIcon::set_menu(std::vector<menu::MenuItem> items) {
    m_menu.set_items(std::move(items));
}

std::optional<menu::MenuSelection> TrayIcon::popup_menu(std::optional<POINT> screen_point) {
    if (m_hwnd == nullptr || m_menu.items().empty()) {
        return std::nullopt;
    }
    menu::PopupOptions options;
    options.owner = m_hwnd;
    options.screen_point = screen_point;
    options.theme = m_theme;
    options.source = menu::MenuSource::Tray;
    options.roles = m_roles;
    if (m_foreground_on_popup) {
        SetForegroundWindow(m_hwnd);
    }
    auto selection = menu::popup_menu(m_menu, options);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    return selection;
}

bool TrayIcon::show_balloon(const NotificationOptions& options) {
    if (!m_visible && !show()) {
        return false;
    }
    NOTIFYICONDATAW data = base_data();
    data.uFlags = NIF_INFO | NIF_SHOWTIP;
    copy_truncated(data.szInfoTitle, std::size(data.szInfoTitle), core::utf8_to_wide(options.title));
    copy_truncated(data.szInfo, std::size(data.szInfo), core::utf8_to_wide(options.body));
    DWORD flags = NIIF_INFO;
    switch (options.level) {
    case NotificationLevel::Info:
        flags = NIIF_INFO;
        break;
    case NotificationLevel::Warning:
        flags = NIIF_WARNING;
        break;
    case NotificationLevel::Error:
        flags = NIIF_ERROR;
        break;
    case NotificationLevel::None:
        flags = NIIF_NONE;
        break;
    }
    if (options.silent) {
        flags |= NIIF_NOSOUND;
    }
    data.dwInfoFlags = flags;
    return notify(NIM_MODIFY, data);
}

std::optional<RECT> TrayIcon::icon_rect() const noexcept {
    if (!m_visible || m_hwnd == nullptr) {
        return std::nullopt;
    }
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(NOTIFYICONIDENTIFIER);
    id.hWnd = m_hwnd;
    id.uID = m_id;
    RECT rect{};
    if (FAILED(Shell_NotifyIconGetRect(&id, &rect))) {
        return std::nullopt;
    }
    return rect;
}

} // namespace boson::native
