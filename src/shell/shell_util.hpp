#pragma once

#include <windows.h>
#include <objbase.h>
#include <propsys.h>

#include <cwchar>
#include <string>
#include <string_view>

namespace boson::shell::detail {

/// Joins the multithreaded apartment for the scope when the thread has no apartment yet. Threads that are
/// already STA (the UI thread) keep their apartment (RPC_E_CHANGED_MODE is success here).
class ScopedCom {
public:
    ScopedCom() noexcept : m_hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_hr)) {
            ::CoUninitialize();
        }
    }
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr;
};

// Local copies of the property keys so no INITGUID translation unit is needed.
inline constexpr PROPERTYKEY kPkeyTitle{{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
inline constexpr PROPERTYKEY kPkeyAppUserModelId{{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
inline constexpr PROPERTYKEY kPkeyIsDestListSeparator{{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 6};
inline constexpr PROPERTYKEY kPkeyToastActivatorClsid{{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 26};

/// PROPVARIANT holding a copy of a string (VT_LPWSTR), cleared on destruction.
class PropString {
public:
    explicit PropString(std::wstring_view value) noexcept {
        ::PropVariantInit(&m_value);
        const std::size_t bytes = (value.size() + 1) * sizeof(wchar_t);
        auto* buffer = static_cast<wchar_t*>(::CoTaskMemAlloc(bytes));
        if (buffer != nullptr) {
            std::wmemcpy(buffer, value.data(), value.size());
            buffer[value.size()] = L'\0';
            m_value.vt = VT_LPWSTR;
            m_value.pwszVal = buffer;
        }
    }
    ~PropString() { ::PropVariantClear(&m_value); }
    PropString(const PropString&) = delete;
    PropString& operator=(const PropString&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_value.vt == VT_LPWSTR; }
    [[nodiscard]] const PROPVARIANT& get() const noexcept { return m_value; }

private:
    PROPVARIANT m_value{};
};

[[nodiscard]] inline std::wstring lower_ascii(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return out;
}

} // namespace boson::shell::detail
