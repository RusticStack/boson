#pragma once

#include <windows.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace boson::core {

/// Checks an HRESULT and logs failures to OutputDebugStringA and std::clog without throwing.
inline bool hr_check(HRESULT hr, std::string_view context) noexcept {
    if (SUCCEEDED(hr)) {
        return true;
    }

    char buffer[512]{};
    const unsigned long code = static_cast<unsigned long>(hr);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "[Boson] HRESULT failure 0x%08lX at: %.*s\n",
        code,
        static_cast<int>(context.size()),
        context.data() ? context.data() : "");

    OutputDebugStringA(buffer);
    std::clog << buffer;
    return false;
}

/// Converts a UTF-8 string_view to a UTF-16 std::wstring using Win32 MultiByteToWideChar.
inline std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        wide.data(),
        required);
    if (written <= 0) {
        return {};
    }

    return wide;
}

/// Converts a UTF-16 wstring_view to a UTF-8 std::string using Win32 WideCharToMultiByte.
inline std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        required,
        nullptr,
        nullptr);
    if (written <= 0) {
        return {};
    }

    return utf8;
}

} // namespace boson::core

#define BOSON_HR_CHECK(expr) (::boson::core::hr_check((expr), #expr))
