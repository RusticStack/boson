#include "desktop_util.hpp"

#include <iostream>

namespace boson::desktop::detail {

namespace {

constexpr wchar_t kHiddenWindowClass[] = L"Boson.Desktop.HiddenWindow";

bool ensure_class_registered() noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHiddenWindowClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "HiddenWindow: RegisterClassExW");
        return false;
    }
    return true;
}

} // namespace

HiddenWindow::HiddenWindow(Handler handler, bool message_only)
    : m_handler(std::move(handler)) {
    if (!ensure_class_registered()) {
        return;
    }
    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kHiddenWindowClass,
        L"",
        WS_POPUP,
        0,
        0,
        0,
        0,
        message_only ? HWND_MESSAGE : nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (m_hwnd == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "HiddenWindow: CreateWindowExW");
        return;
    }
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HiddenWindow::window_proc));
}

HiddenWindow::~HiddenWindow() {
    if (m_hwnd != nullptr) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK HiddenWindow::window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    auto* self = reinterpret_cast<HiddenWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && self->m_handler) {
        // Never let a C++ exception unwind through the OS window procedure.
        try {
            if (const auto handled = self->m_handler(msg, w_param, l_param); handled.has_value()) {
                return *handled;
            }
        } catch (const std::exception& ex) {
            OutputDebugStringA("[Boson] HiddenWindow handler threw: ");
            OutputDebugStringA(ex.what());
            OutputDebugStringA("\n");
            std::clog << "[Boson] HiddenWindow handler threw: " << ex.what() << '\n';
        } catch (...) {
            OutputDebugStringA("[Boson] HiddenWindow handler threw a non-standard exception\n");
        }
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

} // namespace boson::desktop::detail
