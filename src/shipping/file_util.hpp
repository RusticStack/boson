#pragma once

// Private helpers shared by the shipping sources. Not installed, not part of the public API.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace boson::shipping::detail {

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] inline UniqueHandle make_handle(HANDLE handle) noexcept {
    return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

[[nodiscard]] inline std::optional<std::string> read_text_file(const std::filesystem::path& path, std::size_t max_bytes = 64u * 1024u * 1024u) {
    const UniqueHandle file = make_handle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 1u << 30));
        if (!ReadFile(file.get(), text.data() + offset, want, &read, nullptr) || read == 0) {
            return std::nullopt;
        }
        offset += read;
    }
    return text;
}

/// @brief Writes "<path>.tmp" and renames it over path, so readers never see a half-written file.
inline bool write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
        const UniqueHandle file = make_handle(CreateFileW(
            tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return false;
        }
        std::size_t offset = 0;
        while (offset < text.size()) {
            DWORD written = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 1u << 30));
            if (!WriteFile(file.get(), text.data() + offset, want, &written, nullptr) || written == 0) {
                return false;
            }
            offset += written;
        }
        FlushFileBuffers(file.get());
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] inline std::string win32_message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
            FORMAT_MESSAGE_FROM_HMODULE,
        GetModuleHandleW(L"winhttp.dll"),
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::string out = "error " + std::to_string(code);
    if (len > 0 && buffer != nullptr) {
        std::wstring_view text{buffer, len};
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
            text.remove_suffix(1);
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::string utf8(static_cast<std::size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), needed, nullptr, nullptr);
            out += ": " + utf8;
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return out;
}

[[nodiscard]] inline std::filesystem::path temp_directory() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD len = GetTempPathW(MAX_PATH + 1, buffer);
    return len == 0 ? std::filesystem::path{L"."} : std::filesystem::path{std::wstring_view{buffer, len}};
}

[[nodiscard]] inline std::filesystem::path known_local_app_data() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return temp_directory();
    }
    return std::filesystem::path{std::wstring_view{buffer, len}};
}

} // namespace boson::shipping::detail
