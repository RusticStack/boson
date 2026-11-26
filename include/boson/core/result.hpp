#pragma once

#include <windows.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include "boson/core/text_convert.hpp"

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

/// Converts a UTF-8 string_view to a UTF-16 std::wstring (SSE2 ASCII fast path, MultiByteToWideChar otherwise).
inline std::wstring utf8_to_wide(std::string_view utf8) {
    std::wstring wide;
    utf8_to_wide_into(utf8, wide);
    return wide;
}

/// Converts a UTF-16 wstring_view to a UTF-8 std::string (SSE2 ASCII fast path, WideCharToMultiByte otherwise).
inline std::string wide_to_utf8(std::wstring_view wide) {
    std::string utf8;
    wide_to_utf8_into(wide, utf8);
    return utf8;
}

} // namespace boson::core

#define BOSON_HR_CHECK(expr) (::boson::core::hr_check((expr), #expr))
