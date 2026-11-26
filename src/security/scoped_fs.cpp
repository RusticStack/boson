#include <boson/security/scoped_fs.hpp>

#include <boson/core/result.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>

namespace boson::security {

namespace {

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct FindCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            FindClose(handle);
        }
    }
};
using UniqueFind = std::unique_ptr<void, FindCloser>;

std::int64_t to_unix_ms(const FILETIME& ft) noexcept {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr std::uint64_t kEpochDelta = 116444736000000000ull; // 1601 -> 1970 in 100 ns
    if (ticks < kEpochDelta) {
        return 0;
    }
    return static_cast<std::int64_t>((ticks - kEpochDelta) / 10000);
}

std::wstring parent_of(std::wstring_view path) {
    const auto slash = path.rfind(L'\\');
    if (slash == std::wstring_view::npos) {
        return {};
    }
    return std::wstring(path.substr(0, slash));
}

bool is_directory(std::wstring_view path) {
    const DWORD attributes = GetFileAttributesW(std::wstring(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

SecurityResult<void> create_directories(const std::wstring& path) {
    std::vector<std::wstring> missing;
    std::wstring current = path;
    while (!current.empty() && !is_directory(current)) {
        missing.push_back(current);
        current = parent_of(current);
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (!CreateDirectoryW(it->c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return std::unexpected(last_win32_error("CreateDirectoryW"));
        }
    }
    return {};
}

SecurityResult<void> remove_tree(const std::wstring& directory) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\*";
    UniqueFind find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (find.get() == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return std::unexpected(last_win32_error("FindFirstFileExW"));
        }
    } else {
        do {
            const std::wstring_view name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = directory + L"\\" + std::wstring(name);
            const bool reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (reparse) {
                    if (!RemoveDirectoryW(child.c_str())) { // removes the junction / link, not its target
                        return std::unexpected(last_win32_error("RemoveDirectoryW (link)"));
                    }
                } else if (auto removed = remove_tree(child); !removed) {
                    return removed;
                }
            } else {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    SetFileAttributesW(child.c_str(), data.dwFileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
                }
                if (!DeleteFileW(child.c_str())) {
                    return std::unexpected(last_win32_error("DeleteFileW"));
                }
            }
        } while (FindNextFileW(find.get(), &data));
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            return std::unexpected(last_win32_error("FindNextFileW"));
        }
    }
    find.reset();
    if (!RemoveDirectoryW(directory.c_str())) {
        return std::unexpected(last_win32_error("RemoveDirectoryW"));
    }
    return {};
}

std::wstring unique_temp_name(const std::wstring& target) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t value = (GetTickCount64() << 20) ^ (static_cast<std::uint64_t>(GetCurrentThreadId()) << 8) ^
                                counter.fetch_add(1, std::memory_order_relaxed);
    wchar_t suffix[40]{};
    swprintf_s(suffix, L".boson-%016llx.tmp", static_cast<unsigned long long>(value));
    return target + suffix;
}

} // namespace

ScopedFs::ScopedFs(std::shared_ptr<FsScope> scope, std::size_t max_read_bytes)
    : m_scope(scope ? std::move(scope) : std::make_shared<FsScope>()), m_max_read_bytes(max_read_bytes) {}

ScopedFs::~ScopedFs() {
    unwatch_all();
}

SecurityResult<void> ScopedFs::write_handle_all(void* handle, std::span<const std::byte> data) const {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + offset, chunk, &written, nullptr)) {
            return std::unexpected(last_win32_error("WriteFile"));
        }
        if (written == 0) {
            return std::unexpected(SecurityError{.code = ERROR_WRITE_FAULT, .message = "WriteFile wrote nothing"});
        }
        offset += written;
    }
    return {};
}

SecurityResult<void> ScopedFs::ensure_parent(const ResolvedPath& target, bool create_dirs) const {
    const std::wstring parent = parent_of(target.win32);
    if (parent.empty() || is_directory(parent)) {
        return {};
    }
    if (!create_dirs) {
        return std::unexpected(make_win32_error(ERROR_PATH_NOT_FOUND, "parent directory does not exist"));
    }
    return create_directories(parent);
}

SecurityResult<std::vector<std::byte>> ScopedFs::read_bytes(std::string_view path) const {
    auto resolved = m_scope->resolve(path, FsAccess::Read);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    const UniqueHandle file(CreateFileW(
        resolved->win32.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(last_win32_error("CreateFileW (read)"));
    }
    if (auto verified = m_scope->verify_handle(file.get(), FsAccess::Read); !verified) {
        return std::unexpected(std::move(verified.error()));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        return std::unexpected(last_win32_error("GetFileSizeEx"));
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > m_max_read_bytes) {
        return std::unexpected(make_win32_error(ERROR_FILE_TOO_LARGE, "file exceeds the read limit"));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 30));
        DWORD read = 0;
        if (!ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr)) {
            return std::unexpected(last_win32_error("ReadFile"));
        }
        if (read == 0) {
            break; // truncated concurrently
        }
        offset += read;
    }
    bytes.resize(offset);
    return bytes;
}

SecurityResult<std::string> ScopedFs::read_text(std::string_view path) const {
    auto bytes = read_bytes(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    if (!is_valid_utf8(text)) {
        return std::unexpected(SecurityError{.code = ERROR_INVALID_DATA, .message = "file is not valid UTF-8 (use read_bytes)"});
    }
    return text;
}

SecurityResult<void> ScopedFs::write_bytes(std::string_view path, std::span<const std::byte> data, bool create_dirs) const {
    auto resolved = m_scope->resolve(path, FsAccess::Write);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    if (is_directory(resolved->win32)) {
        return std::unexpected(make_win32_error(ERROR_DIRECTORY_NOT_SUPPORTED, "target is a directory"));
    }
    if (auto parent = ensure_parent(*resolved, create_dirs); !parent) {
        return parent;
    }
    const std::wstring temp = unique_temp_name(resolved->win32);
    {
        const UniqueHandle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) {
            return std::unexpected(last_win32_error("CreateFileW (write)"));
        }
        auto verified = m_scope->verify_handle(file.get(), FsAccess::Write);
        auto written = verified ? write_handle_all(file.get(), data) : SecurityResult<void>(std::unexpected(verified.error()));
        if (!written) {
            FILE_DISPOSITION_INFO dispose{.DeleteFile = TRUE};
            SetFileInformationByHandle(file.get(), FileDispositionInfo, &dispose, sizeof(dispose));
            return written;
        }
    }
    if (!MoveFileExW(temp.c_str(), resolved->win32.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const SecurityError error = last_win32_error("MoveFileExW (replace)");
        DeleteFileW(temp.c_str());
        return std::unexpected(error);
    }
    return {};
}

SecurityResult<void> ScopedFs::write_text(std::string_view path, std::string_view text, bool create_dirs) const {
    return write_bytes(path, as_bytes(text), create_dirs);
}

SecurityResult<void> ScopedFs::append(std::string_view path, std::span<const std::byte> data, bool create_dirs) const {
    auto resolved = m_scope->resolve(path, FsAccess::Write);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    if (auto parent = ensure_parent(*resolved, create_dirs); !parent) {
        return parent;
    }
    const UniqueHandle file(CreateFileW(
        resolved->win32.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(last_win32_error("CreateFileW (append)"));
    }
    if (auto verified = m_scope->verify_handle(file.get(), FsAccess::Write); !verified) {
        return verified;
    }
    return write_handle_all(file.get(), data);
}

SecurityResult<bool> ScopedFs::exists(std::string_view path) const {
    auto resolved = m_scope->resolve(path, FsAccess::Read);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    return GetFileAttributesW(resolved->win32.c_str()) != INVALID_FILE_ATTRIBUTES;
}

SecurityResult<FsStat> ScopedFs::stat(std::string_view path) const {
    auto resolved = m_scope->resolve(path, FsAccess::Read);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    FsStat out{.path = resolved->display};
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(resolved->win32.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return out;
        }
        return std::unexpected(make_win32_error(error, "GetFileAttributesExW"));
    }
    out.exists = true;
    out.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.is_file = !out.is_dir;
    out.readonly = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    out.size = out.is_dir ? 0 : ((static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
    out.modified_ms = to_unix_ms(data.ftLastWriteTime);
    out.created_ms = to_unix_ms(data.ftCreationTime);
    out.accessed_ms = to_unix_ms(data.ftLastAccessTime);
    const std::wstring lexical_extended = L"\\\\?\\" + resolved->lexical;
    const DWORD link_attributes = GetFileAttributesW(lexical_extended.c_str());
    out.is_symlink = link_attributes != INVALID_FILE_ATTRIBUTES && (link_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return out;
}

SecurityResult<std::vector<FsEntry>> ScopedFs::read_dir(std::string_view path) const {
    auto resolved = m_scope->resolve(path, FsAccess::Read);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    std::vector<FsEntry> entries;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = resolved->win32 + L"\\*";
    const UniqueFind find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (find.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return entries;
        }
        return std::unexpected(make_win32_error(error, "FindFirstFileExW"));
    }
    std::string base = resolved->display;
    if (!base.ends_with('\\')) {
        base.push_back('\\');
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        FsEntry entry{.name = core::wide_to_utf8(name)};
        entry.path = base + entry.name;
        entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.is_file = !entry.is_dir;
        entry.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.size = entry.is_dir ? 0 : ((static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
        entry.modified_ms = to_unix_ms(data.ftLastWriteTime);
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find.get(), &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        return std::unexpected(last_win32_error("FindNextFileW"));
    }
    std::ranges::sort(entries, {}, &FsEntry::name);
    return entries;
}

SecurityResult<void> ScopedFs::mkdir(std::string_view path, bool recursive) const {
    auto resolved = m_scope->resolve(path, FsAccess::Write);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    if (recursive) {
        return create_directories(resolved->win32);
    }
    if (!CreateDirectoryW(resolved->win32.c_str(), nullptr)) {
        return std::unexpected(last_win32_error("CreateDirectoryW"));
    }
    return {};
}

SecurityResult<void> ScopedFs::remove(std::string_view path, bool recursive) const {
    auto resolved = m_scope->resolve_entry(path, FsAccess::Write);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    if (resolved->is_root) {
        return std::unexpected(SecurityError{.code = ERROR_ACCESS_DENIED, .message = "permission denied: a scope root cannot be removed"});
    }
    const DWORD attributes = GetFileAttributesW(resolved->win32.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(last_win32_error("GetFileAttributesW"));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (recursive && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            return remove_tree(resolved->win32);
        }
        if (!RemoveDirectoryW(resolved->win32.c_str())) {
            return std::unexpected(last_win32_error("RemoveDirectoryW"));
        }
        return {};
    }
    if (!DeleteFileW(resolved->win32.c_str())) {
        return std::unexpected(last_win32_error("DeleteFileW"));
    }
    return {};
}

SecurityResult<void> ScopedFs::rename(std::string_view from, std::string_view to, bool overwrite) const {
    auto source = m_scope->resolve_entry(from, FsAccess::Write);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    auto target = m_scope->resolve_entry(to, FsAccess::Write);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    if (source->is_root || target->is_root) {
        return std::unexpected(SecurityError{.code = ERROR_ACCESS_DENIED, .message = "permission denied: a scope root cannot be renamed or replaced"});
    }
    const DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(source->win32.c_str(), target->win32.c_str(), flags)) {
        return std::unexpected(last_win32_error("MoveFileExW"));
    }
    return {};
}

SecurityResult<void> ScopedFs::copy(std::string_view from, std::string_view to, bool overwrite) const {
    auto source = m_scope->resolve(from, FsAccess::Read);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    auto target = m_scope->resolve(to, FsAccess::Write);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    if (is_directory(source->win32)) {
        return std::unexpected(make_win32_error(ERROR_DIRECTORY_NOT_SUPPORTED, "copy: source is a directory"));
    }
    if (target->is_root || is_directory(target->win32)) {
        return std::unexpected(make_win32_error(ERROR_DIRECTORY_NOT_SUPPORTED, "copy: target is a directory"));
    }
    BOOL cancel = FALSE;
    if (!CopyFileExW(source->win32.c_str(), target->win32.c_str(), nullptr, nullptr, &cancel, overwrite ? 0 : COPY_FILE_FAIL_IF_EXISTS)) {
        return std::unexpected(last_win32_error("CopyFileExW"));
    }
    return {};
}

SecurityResult<std::uint64_t> ScopedFs::watch(std::string_view path, bool recursive, DirectoryWatcher::Callback callback) {
    auto resolved = m_scope->resolve(path, FsAccess::Read);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    if (!is_directory(resolved->win32)) {
        return std::unexpected(make_win32_error(ERROR_DIRECTORY, "watch: not a directory"));
    }
    std::uint64_t id = 0;
    {
        std::scoped_lock lock(m_watch_mutex);
        if (m_watches.size() >= kMaxWatches) {
            return std::unexpected(make_win32_error(ERROR_TOO_MANY_OPEN_FILES, "watch: too many active watches"));
        }
        id = m_next_watch_id++;
    }
    // A non-recursive scope must not leak events from sub-directories.
    auto watcher = DirectoryWatcher::start(resolved->win32, recursive && resolved->scope_recursive, id, std::move(callback));
    if (!watcher) {
        return std::unexpected(std::move(watcher.error()));
    }
    std::scoped_lock lock(m_watch_mutex);
    m_watches.emplace(id, std::move(*watcher));
    return id;
}

bool ScopedFs::unwatch(std::uint64_t id) {
    std::unique_ptr<DirectoryWatcher> watcher;
    {
        std::scoped_lock lock(m_watch_mutex);
        const auto it = m_watches.find(id);
        if (it == m_watches.end()) {
            return false;
        }
        watcher = std::move(it->second);
        m_watches.erase(it);
    }
    watcher.reset(); // waits for an in-flight callback outside the lock
    return true;
}

void ScopedFs::unwatch_all() {
    std::unordered_map<std::uint64_t, std::unique_ptr<DirectoryWatcher>> watches;
    {
        std::scoped_lock lock(m_watch_mutex);
        watches.swap(m_watches);
    }
    watches.clear();
}

std::size_t ScopedFs::watch_count() const {
    std::scoped_lock lock(m_watch_mutex);
    return m_watches.size();
}

} // namespace boson::security
