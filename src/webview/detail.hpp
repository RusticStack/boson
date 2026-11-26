#pragma once

// Internal helpers shared by the src/webview/*.cpp translation units. Not a public header.

#include <windows.h>
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview::detail {

using Microsoft::WRL::ComPtr;

/// @brief Frees a CoTaskMem string and returns it as UTF-8 (empty when hr failed or raw is null).
inline std::string take_utf8(HRESULT hr, wchar_t* raw, std::string_view context) {
    std::string out;
    if (core::hr_check(hr, context) && raw != nullptr) {
        out = core::wide_to_utf8(raw);
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return out;
}

/// @brief Calls a COM string getter (HRESULT get_X(LPWSTR*)) and returns the value as UTF-8.
/// Use this instead of take_utf8(obj->get_X(&raw), raw, ...): argument evaluation order is unspecified, so raw
/// may be read before the getter writes it.
template <typename Getter>
[[nodiscard]] std::string read_utf8(Getter&& getter, std::string_view context) {
    wchar_t* raw = nullptr;
    const HRESULT hr = getter(&raw);
    return take_utf8(hr, raw, context);
}

/// @brief Frees a CoTaskMem string and returns it as UTF-16 (empty when hr failed or raw is null).
inline std::wstring take_wide(HRESULT hr, wchar_t* raw, std::string_view context) {
    std::wstring out;
    if (core::hr_check(hr, context) && raw != nullptr) {
        out = raw;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return out;
}

/// @brief read_utf8 for UTF-16 results.
template <typename Getter>
[[nodiscard]] std::wstring read_wide(Getter&& getter, std::string_view context) {
    wchar_t* raw = nullptr;
    const HRESULT hr = getter(&raw);
    return take_wide(hr, raw, context);
}

/// @brief QueryInterface on the WebView's ICoreWebView2; null when not ready or unsupported by the runtime.
template <typename Interface>
[[nodiscard]] ComPtr<Interface> query(const core::WebView& webview) noexcept {
    ComPtr<Interface> out;
    if (ICoreWebView2* core = webview.core_webview(); core != nullptr) {
        if (FAILED(core->QueryInterface(IID_PPV_ARGS(&out)))) {
            out.Reset();
        }
    }
    return out;
}

/// @brief Like query(), but logs which interface version was missing.
template <typename Interface>
[[nodiscard]] ComPtr<Interface> require(const core::WebView& webview, std::string_view what) noexcept {
    ComPtr<Interface> out = query<Interface>(webview);
    if (!out && webview.core_webview() != nullptr) {
        core::hr_check(E_NOINTERFACE, what);
    }
    return out;
}

/// @brief The ICoreWebView2Environment that owns webview (ICoreWebView2_2::get_Environment).
[[nodiscard]] inline ComPtr<ICoreWebView2Environment> environment_of(const core::WebView& webview) noexcept {
    ComPtr<ICoreWebView2Environment> env;
    if (const auto webview2 = query<ICoreWebView2_2>(webview)) {
        if (!BOSON_HR_CHECK(webview2->get_Environment(&env))) {
            env.Reset();
        }
    }
    return env;
}

/// @brief Reads a whole IStream from its beginning into memory.
[[nodiscard]] inline std::vector<std::byte> read_stream(IStream* stream) {
    std::vector<std::byte> bytes;
    if (stream == nullptr) {
        return bytes;
    }
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr); // Streams that cannot seek are already at the start.

    STATSTG stat{};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 &&
        stat.cbSize.QuadPart < (std::uint64_t{1} << 31)) {
        const auto size = static_cast<std::size_t>(stat.cbSize.QuadPart);
        bytes.resize(size);
        std::size_t total = 0;
        while (total < size) {
            ULONG read = 0;
            const HRESULT hr = stream->Read(bytes.data() + total, static_cast<ULONG>(size - total), &read);
            if (FAILED(hr) || read == 0) {
                break;
            }
            total += read;
        }
        bytes.resize(total);
        return bytes;
    }

    std::vector<std::byte> chunk(64 * 1024);
    for (;;) {
        ULONG read = 0;
        const HRESULT hr = stream->Read(chunk.data(), static_cast<ULONG>(chunk.size()), &read);
        if (FAILED(hr) || read == 0) {
            break;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read);
    }
    return bytes;
}

} // namespace boson::webview::detail
