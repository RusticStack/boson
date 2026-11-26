#include <boson/store/store.hpp>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/core/result.hpp>
#include <boson/security/common.hpp>

namespace boson::store {

namespace {

namespace fs = std::filesystem;

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] UniqueHandle make_handle(HANDLE handle) noexcept {
    return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

struct CoTaskFree {
    void operator()(wchar_t* p) const noexcept { CoTaskMemFree(p); }
};

[[nodiscard]] StoreError win32_error(DWORD code, std::string_view context, const fs::path& path) {
    const security::SecurityError base = security::make_win32_error(code, context);
    std::string message = base.message;
    if (!path.empty()) {
        message += " (" + core::wide_to_utf8(path.wstring()) + ")";
    }
    return StoreError{.code = static_cast<std::uint32_t>(code), .message = std::move(message)};
}

[[nodiscard]] bool is_valid_component(std::wstring_view name) noexcept {
    if (name.empty() || name == L"." || name == L"..") {
        return false;
    }
    for (const wchar_t ch : name) {
        if (ch < 32 || ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
            ch == L'<' || ch == L'>' || ch == L'|') {
            return false;
        }
    }
    return name.back() != L'.' && name.back() != L' ';
}

[[nodiscard]] std::wstring utc_stamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    std::array<wchar_t, 32> buffer{};
    std::swprintf(buffer.data(), buffer.size(), L"%04u%02u%02uT%02u%02u%02u%03uZ",
        static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds));
    return std::wstring{buffer.data()};
}

[[nodiscard]] fs::path sibling(const fs::path& path, std::wstring_view suffix) {
    fs::path out = path;
    out += suffix;
    return out;
}

/// Files in path's directory whose name is "<filename><middle>..." (case-insensitive prefix match).
[[nodiscard]] std::vector<fs::path> siblings_with_prefix(const fs::path& path, std::wstring_view middle, std::wstring_view suffix) {
    std::vector<fs::path> out;
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path{L"."};
    const std::wstring prefix = path.filename().wstring() + std::wstring{middle};
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.size() <= prefix.size() + suffix.size()) {
            continue;
        }
        if (CompareStringOrdinal(name.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
            continue;
        }
        if (!suffix.empty() &&
            CompareStringOrdinal(name.data() + name.size() - suffix.size(), static_cast<int>(suffix.size()), suffix.data(),
                static_cast<int>(suffix.size()), TRUE) != CSTR_EQUAL) {
            continue;
        }
        std::error_code type_ec;
        if (it->is_regular_file(type_ec)) {
            out.push_back(it->path());
        }
    }
    return out;
}

[[nodiscard]] bool is_transient(DWORD code) noexcept {
    return code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION ||
        code == ERROR_UNABLE_TO_REMOVE_REPLACED;
}

std::atomic<std::uint64_t> g_temp_counter{0};

} // namespace

std::string_view to_string(LoadStatus status) noexcept {
    switch (status) {
    case LoadStatus::Loaded: return "loaded";
    case LoadStatus::Missing: return "missing";
    case LoadStatus::Corrupt: return "corrupt";
    case LoadStatus::RecoveredFromBackup: return "recovered_from_backup";
    case LoadStatus::Unreadable: return "unreadable";
    case LoadStatus::InMemory: return "in_memory";
    }
    return "unknown";
}

StoreResult<void> write_file_atomic(const fs::path& path, std::span<const std::byte> bytes, const AtomicWriteOptions& options) {
    if (path.empty() || !path.has_filename()) {
        return std::unexpected(StoreError{.code = ERROR_INVALID_PARAMETER, .message = "write_file_atomic: empty path"});
    }
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(StoreError{
                .code = static_cast<std::uint32_t>(ec.value()),
                .message = "cannot create " + core::wide_to_utf8(path.parent_path().wstring()) + ": " + ec.message()});
        }
    }

    const fs::path temp = sibling(path,
        L"." + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(g_temp_counter.fetch_add(1, std::memory_order_relaxed) + 1) + L".tmp");
    {
        const UniqueHandle file = make_handle(CreateFileW(
            temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return std::unexpected(win32_error(GetLastError(), "CreateFileW(temp)", temp));
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 30));
            if (!WriteFile(file.get(), bytes.data() + offset, want, &written, nullptr) || written == 0) {
                const DWORD code = GetLastError();
                static_cast<void>(DeleteFileW(temp.c_str()));
                return std::unexpected(win32_error(code == 0 ? ERROR_WRITE_FAULT : code, "WriteFile", temp));
            }
            offset += written;
        }
        if (options.flush && !FlushFileBuffers(file.get())) {
            const DWORD code = GetLastError();
            static_cast<void>(DeleteFileW(temp.c_str()));
            return std::unexpected(win32_error(code, "FlushFileBuffers", temp));
        }
    }

    if (options.before_replace) {
        options.before_replace(temp);
    }

    const fs::path backup = detail::backup_path_of(path);
    DWORD code = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (ReplaceFileW(path.c_str(), temp.c_str(), options.keep_backup ? backup.c_str() : nullptr,
                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return {};
        }
        code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
            // No target yet, or (1176, no backup requested) the target is already gone and the replacement
            // kept its temp name: a plain move finishes the job either way.
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return {};
            }
            code = GetLastError();
        } else if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            // 1177: the original now has the backup name, the replacement still has the temp name.
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return {};
            }
            code = GetLastError();
            static_cast<void>(MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH));
            break;
        }
        if (!is_transient(code)) {
            break;
        }
        Sleep(static_cast<DWORD>(5 + attempt * 10));
    }
    static_cast<void>(DeleteFileW(temp.c_str()));
    return std::unexpected(win32_error(code, "ReplaceFileW", path));
}

StoreResult<fs::path> default_store_path(std::wstring_view app_name, std::wstring_view file_name) {
    if (!is_valid_component(app_name) || !is_valid_component(file_name)) {
        return std::unexpected(StoreError{
            .code = ERROR_INVALID_NAME, .message = "default_store_path: app_name and file_name must be single path components"});
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    const std::unique_ptr<wchar_t, CoTaskFree> folder(raw);
    if (!BOSON_HR_CHECK(hr) || !folder) {
        return std::unexpected(StoreError{.code = static_cast<std::uint32_t>(hr), .message = "SHGetKnownFolderPath(LocalAppData) failed"});
    }
    return fs::path{folder.get()} / fs::path{app_name} / fs::path{file_name};
}

namespace detail {

namespace {
// Re-entrancy guard: the stores whose writer lock this thread holds (nesting depth is tiny in practice).
constexpr std::size_t kMaxNesting = 16;
thread_local std::array<const void*, kMaxNesting> t_writing{};
thread_local std::size_t t_writing_count = 0;
} // namespace

bool is_writing(const void* store) noexcept {
    for (std::size_t i = 0; i < t_writing_count; ++i) {
        if (t_writing[i] == store) {
            return true;
        }
    }
    return false;
}

WriteScope::WriteScope(const void* store) noexcept {
    if (t_writing_count < kMaxNesting) {
        t_writing[t_writing_count++] = store;
        m_pushed = true;
    }
}

WriteScope::~WriteScope() {
    if (m_pushed && t_writing_count > 0) {
        --t_writing_count;
    }
}

StoreResult<std::vector<std::string>> diff_top_level(std::string_view old_json, std::string_view new_json) {
    std::map<std::string, glz::raw_json> before;
    std::map<std::string, glz::raw_json> after;
    if (const auto ec = glz::read_json(before, old_json)) {
        return std::unexpected(StoreError{.code = ERROR_INVALID_DATA, .message = glz::format_error(ec, old_json)});
    }
    if (const auto ec = glz::read_json(after, new_json)) {
        return std::unexpected(StoreError{.code = ERROR_INVALID_DATA, .message = glz::format_error(ec, new_json)});
    }
    std::vector<std::string> keys;
    auto a = before.begin();
    auto b = after.begin();
    while (a != before.end() || b != after.end()) {
        if (b == after.end() || (a != before.end() && a->first < b->first)) {
            keys.push_back(a->first);
            ++a;
        } else if (a == before.end() || b->first < a->first) {
            keys.push_back(b->first);
            ++b;
        } else {
            if (a->second.str != b->second.str) {
                keys.push_back(a->first);
            }
            ++a;
            ++b;
        }
    }
    return keys;
}

ReadResult read_file(const fs::path& path, std::size_t max_bytes) {
    ReadResult out;
    const UniqueHandle file = make_handle(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            out.kind = ReadResult::Kind::Missing;
            return out;
        }
        out.kind = ReadResult::Kind::Error;
        out.error = win32_error(code, "CreateFileW", path);
        return out;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
        out.kind = ReadResult::Kind::Error;
        out.error = win32_error(GetLastError(), "GetFileSizeEx", path);
        return out;
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        out.kind = ReadResult::Kind::TooLarge;
        out.error = StoreError{.code = ERROR_FILE_TOO_LARGE, .message = "file is larger than max_file_bytes"};
        return out;
    }
    out.text.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < out.text.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.text.size() - offset, 1u << 30));
        if (!ReadFile(file.get(), out.text.data() + offset, want, &read, nullptr)) {
            out.kind = ReadResult::Kind::Error;
            out.error = win32_error(GetLastError(), "ReadFile", path);
            out.text.clear();
            return out;
        }
        if (read == 0) {
            break; // truncated underneath us: parse what is there
        }
        offset += read;
    }
    out.text.resize(offset);
    if (out.text.starts_with("\xEF\xBB\xBF")) {
        out.text.erase(0, 3);
    }
    out.kind = ReadResult::Kind::Ok;
    return out;
}

fs::path backup_path_of(const fs::path& path) {
    return sibling(path, L".bak");
}

StoreResult<fs::path> backup_corrupt(const fs::path& path, std::size_t keep) {
    if (keep == 0) {
        if (!DeleteFileW(path.c_str())) {
            return std::unexpected(win32_error(GetLastError(), "DeleteFileW", path));
        }
        return fs::path{};
    }
    const std::wstring stamp = utc_stamp();
    fs::path target;
    DWORD code = ERROR_SUCCESS;
    bool done = false;
    for (int n = 0; n < 100 && !done; ++n) {
        target = sibling(path, L".corrupt-" + stamp + (n == 0 ? std::wstring{} : L"-" + std::to_wstring(n)));
        if (MoveFileExW(path.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            done = true;
            break;
        }
        code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) {
            // Locked (no FILE_SHARE_DELETE by another reader): keep a copy at least.
            if (CopyFileW(path.c_str(), target.c_str(), TRUE)) {
                done = true;
            } else {
                code = GetLastError();
            }
            break;
        }
    }
    if (!done) {
        return std::unexpected(win32_error(code, "MoveFileExW(corrupt backup)", path));
    }
    auto backups = siblings_with_prefix(path, L".corrupt-", {});
    if (backups.size() > keep) {
        std::sort(backups.begin(), backups.end()); // UTC timestamps sort chronologically
        for (std::size_t i = 0; i + keep < backups.size(); ++i) {
            static_cast<void>(DeleteFileW(backups[i].c_str()));
        }
    }
    return target;
}

std::size_t remove_stale_temp_files(const fs::path& path) noexcept {
    try {
        std::size_t removed = 0;
        for (const auto& temp : siblings_with_prefix(path, L".", L".tmp")) {
            if (DeleteFileW(temp.c_str())) {
                ++removed;
            }
        }
        return removed;
    } catch (...) {
        return 0;
    }
}

bool valid_utf8(std::string_view text) noexcept {
    return security::is_valid_utf8(text);
}

} // namespace detail

} // namespace boson::store
