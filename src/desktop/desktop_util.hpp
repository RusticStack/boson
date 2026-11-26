#pragma once

// Private helpers shared by the src/desktop/*.cpp translation units. Not installed.

#include <windows.h>
#include <objbase.h>

#include <boson/core/result.hpp>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace boson::desktop::detail {

/// @brief Initialises COM on the calling thread for the lifetime of the object when the thread has no
/// apartment yet (TaskRunner pool threads). A thread that is already MTA/STA is left as it is.
class ScopedCom {
public:
    ScopedCom() noexcept
        : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    /// true when COM is usable on this thread (initialised here or already initialised in another mode).
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr = E_FAIL;
};

/// @brief "System message (0x8007xxxx)" for an HRESULT.
[[nodiscard]] inline std::string hresult_message(HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::string text;
    if (len != 0 && buffer != nullptr) {
        std::wstring_view view{buffer, len};
        while (!view.empty() && (view.back() == L'\n' || view.back() == L'\r' || view.back() == L' ' || view.back() == L'.')) {
            view.remove_suffix(1);
        }
        text = core::wide_to_utf8(view);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    if (text.empty()) {
        return std::string("error ") + code;
    }
    return text + " (" + code + ")";
}

[[nodiscard]] inline std::string win32_message(DWORD code) {
    return hresult_message(HRESULT_FROM_WIN32(code));
}

[[nodiscard]] inline std::string last_error_message(std::string_view context) {
    const DWORD code = GetLastError();
    std::string out{context};
    out += ": ";
    out += win32_message(code);
    return out;
}

/// @brief hr_check() plus a formatted message for std::expected errors.
[[nodiscard]] inline std::optional<std::string> hr_error(HRESULT hr, std::string_view context) {
    if (core::hr_check(hr, context)) {
        return std::nullopt;
    }
    std::string out{context};
    out += ": ";
    out += hresult_message(hr);
    return out;
}

[[nodiscard]] inline char ascii_lower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] inline std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(ascii_lower(ch));
    }
    return out;
}

[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// @brief Invisible window that receives messages on the thread that created it. A top-level popup (not
/// message-only) also receives broadcasts such as WM_DISPLAYCHANGE and WM_SETTINGCHANGE. It is never
/// shown and never activated.
class HiddenWindow {
public:
    using Handler = std::function<std::optional<LRESULT>(UINT msg, WPARAM w_param, LPARAM l_param)>;

    HiddenWindow(Handler handler, bool message_only);
    ~HiddenWindow();
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;

    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

    Handler m_handler;
    HWND m_hwnd = nullptr;
};

} // namespace boson::desktop::detail
