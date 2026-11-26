#include <boson/security/fs_scope.hpp>

#include <boson/core/result.hpp>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>

namespace boson::security {

namespace {

constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";

SecurityError denied(std::string_view why) {
    std::string message = "permission denied: ";
    message.append(why);
    return SecurityError{.code = ERROR_ACCESS_DENIED, .message = std::move(message)};
}

SecurityError invalid_name(std::string_view why) {
    std::string message = "invalid path: ";
    message.append(why);
    return SecurityError{.code = ERROR_INVALID_NAME, .message = std::move(message)};
}

bool iequals_wide(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool is_drive_letter(wchar_t ch) noexcept {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

SecurityResult<std::wstring> final_path_of_handle(HANDLE handle) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return std::unexpected(last_win32_error("GetFinalPathNameByHandleW"));
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            while (buffer.size() > kExtendedPrefix.size() && buffer.back() == L'\\') {
                buffer.pop_back();
            }
            return buffer;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
    return std::unexpected(SecurityError{.code = ERROR_FILENAME_EXCED_RANGE, .message = "GetFinalPathNameByHandleW: path too long"});
}

std::wstring to_extended(std::wstring_view normalized) {
    std::wstring out(kExtendedPrefix);
    out.append(normalized);
    return out;
}

/// "C:\a\b" -> {"C:\a", "b"}; drive root has no parent.
bool split_last(std::wstring_view path, std::wstring_view& parent, std::wstring_view& leaf) noexcept {
    const auto slash = path.rfind(L'\\');
    if (slash == std::wstring_view::npos || slash + 1 >= path.size()) {
        return false;
    }
    leaf = path.substr(slash + 1);
    parent = path.substr(0, slash);
    if (parent.size() == 2) {
        parent = path.substr(0, 3); // keep "C:\"
    }
    return !leaf.empty();
}

bool is_not_found(DWORD code) noexcept {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

/// Final path of the deepest existing ancestor + the non-existent tail.
SecurityResult<std::wstring> resolve_following(std::wstring_view normalized) {
    std::wstring current(normalized);
    std::vector<std::wstring> tail;
    for (;;) {
        auto final_path = FsScope::final_path_of(current);
        if (final_path) {
            std::wstring out = std::move(*final_path);
            for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
                out.push_back(L'\\');
                out.append(*it);
            }
            return out;
        }
        if (!is_not_found(final_path.error().code)) {
            return std::unexpected(std::move(final_path.error()));
        }
        std::wstring_view parent;
        std::wstring_view leaf;
        if (!split_last(current, parent, leaf)) {
            return std::unexpected(std::move(final_path.error()));
        }
        tail.emplace_back(leaf);
        current = std::wstring(parent);
    }
}

SecurityResult<std::wstring> known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        if (raw != nullptr) {
            CoTaskMemFree(raw);
        }
        return std::unexpected(make_win32_error(static_cast<std::uint32_t>(HRESULT_CODE(hr)), "SHGetKnownFolderPath"));
    }
    std::wstring out(raw);
    CoTaskMemFree(raw);
    return out;
}

} // namespace

// ---- Lexical checks ---------------------------------------------------------------------------------------

bool FsScope::is_reserved_device_name(std::wstring_view component) noexcept {
    // The base name before the first '.', trailing spaces ignored: "NUL.txt" and "com1 .log" are devices.
    std::wstring_view base = component.substr(0, component.find(L'.'));
    while (!base.empty() && base.back() == L' ') {
        base.remove_suffix(1);
    }
    static constexpr std::array<std::wstring_view, 6> kFixed = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (const auto name : kFixed) {
        if (iequals_wide(base, name)) {
            return true;
        }
    }
    if (base.size() == 4) {
        const std::wstring_view stem = base.substr(0, 3);
        const wchar_t digit = base[3];
        const bool is_digit = (digit >= L'0' && digit <= L'9') || digit == L'\u00B9' || digit == L'\u00B2' || digit == L'\u00B3';
        if (is_digit && (iequals_wide(stem, L"COM") || iequals_wide(stem, L"LPT"))) {
            return true;
        }
    }
    return false;
}

SecurityResult<std::wstring> FsScope::normalize_lexically(std::wstring_view input) {
    if (input.empty()) {
        return std::unexpected(invalid_name("empty path"));
    }
    if (input.size() > 32000) {
        return std::unexpected(SecurityError{.code = ERROR_FILENAME_EXCED_RANGE, .message = "invalid path: too long"});
    }
    std::wstring path(input);
    std::ranges::replace(path, L'/', L'\\');
    if (path.find(L'\0') != std::wstring::npos) {
        return std::unexpected(invalid_name("embedded NUL"));
    }
    if (path.starts_with(L"\\\\") || path.starts_with(L"\\??\\")) {
        return std::unexpected(denied("UNC, device and namespace paths are not allowed"));
    }
    if (path.size() < 3 || !is_drive_letter(path[0]) || path[1] != L':' || path[2] != L'\\') {
        return std::unexpected(invalid_name("expected an absolute path like C:\\dir\\file"));
    }

    std::vector<std::wstring_view> parts;
    std::wstring_view rest = std::wstring_view(path).substr(3);
    while (!rest.empty()) {
        const auto slash = rest.find(L'\\');
        const std::wstring_view component = rest.substr(0, slash);
        rest = slash == std::wstring_view::npos ? std::wstring_view{} : rest.substr(slash + 1);
        if (component.empty() || component == L".") {
            continue;
        }
        if (component == L"..") {
            if (parts.empty()) {
                return std::unexpected(denied("'..' escapes the drive root"));
            }
            parts.pop_back();
            continue;
        }
        for (const wchar_t ch : component) {
            if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' || ch == L'?' || ch == L'*') {
                return std::unexpected(invalid_name("wildcard or control character"));
            }
            if (ch == L':') {
                return std::unexpected(denied("alternate data streams are not allowed"));
            }
        }
        if (component.back() == L'.' || component.back() == L' ') {
            return std::unexpected(invalid_name("component ends with '.' or ' '"));
        }
        if (is_reserved_device_name(component)) {
            return std::unexpected(denied("reserved device name"));
        }
        parts.push_back(component);
    }

    std::wstring out;
    out.reserve(path.size());
    out.push_back(static_cast<wchar_t>(std::towupper(path[0])));
    out.append(L":\\");
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.push_back(L'\\');
        }
        out.append(parts[i]);
    }
    return out;
}

SecurityResult<std::wstring> FsScope::final_path_of(std::wstring_view path) {
    const std::wstring extended = path.starts_with(kExtendedPrefix) ? std::wstring(path) : to_extended(path);
    const UniqueHandle handle(CreateFileW(
        extended.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(last_win32_error("CreateFileW"));
    }
    return final_path_of_handle(handle.get());
}

std::wstring FsScope::display_form(std::wstring_view final_path) {
    if (final_path.starts_with(kExtendedUncPrefix)) {
        std::wstring out = L"\\\\";
        out.append(final_path.substr(kExtendedUncPrefix.size()));
        return out;
    }
    if (final_path.starts_with(kExtendedPrefix)) {
        std::wstring out(final_path.substr(kExtendedPrefix.size()));
        if (out.size() == 2 && out[1] == L':') {
            out.push_back(L'\\');
        }
        return out;
    }
    return std::wstring(final_path);
}

SecurityResult<std::wstring> FsScope::base_dir_path(FsBaseDir base, std::string_view app_name) {
    SecurityResult<std::wstring> folder;
    switch (base) {
    case FsBaseDir::AppData:
        folder = known_folder(FOLDERID_RoamingAppData);
        break;
    case FsBaseDir::AppLocalData:
        folder = known_folder(FOLDERID_LocalAppData);
        break;
    case FsBaseDir::Documents:
        folder = known_folder(FOLDERID_Documents);
        break;
    case FsBaseDir::Downloads:
        folder = known_folder(FOLDERID_Downloads);
        break;
    case FsBaseDir::Desktop:
        folder = known_folder(FOLDERID_Desktop);
        break;
    case FsBaseDir::Pictures:
        folder = known_folder(FOLDERID_Pictures);
        break;
    case FsBaseDir::Music:
        folder = known_folder(FOLDERID_Music);
        break;
    case FsBaseDir::Videos:
        folder = known_folder(FOLDERID_Videos);
        break;
    case FsBaseDir::Temp: {
        std::array<wchar_t, MAX_PATH + 1> buffer{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0 || length > buffer.size()) {
            return std::unexpected(last_win32_error("GetTempPathW"));
        }
        folder = std::wstring(buffer.data(), length);
        break;
    }
    }
    if (!folder) {
        return folder;
    }
    if (base == FsBaseDir::AppData || base == FsBaseDir::AppLocalData) {
        const std::wstring app = core::utf8_to_wide(app_name);
        if (app.empty() || app.find_first_of(L"\\/:") != std::wstring::npos || app == L"." || app == L".." ||
            is_reserved_device_name(app)) {
            return std::unexpected(invalid_name("app name must be a single folder name"));
        }
        std::wstring dir = *folder;
        if (!dir.ends_with(L'\\')) {
            dir.push_back(L'\\');
        }
        dir.append(app);
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return std::unexpected(last_win32_error("CreateDirectoryW"));
        }
        return dir;
    }
    return folder;
}

// ---- Roots --------------------------------------------------------------------------------------------------

SecurityResult<std::string> FsScope::allow(std::string_view utf8_path, bool read_only, bool recursive, std::string label) {
    if (!is_valid_utf8(utf8_path)) {
        return std::unexpected(invalid_name("not UTF-8"));
    }
    return allow(std::wstring_view(core::utf8_to_wide(utf8_path)), read_only, recursive, std::move(label));
}

SecurityResult<std::string> FsScope::allow(std::wstring_view path, bool read_only, bool recursive, std::string label) {
    auto normalized = normalize_lexically(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    auto final_path = final_path_of(*normalized);
    if (!final_path) {
        return std::unexpected(std::move(final_path.error()));
    }
    const DWORD attributes = GetFileAttributesW(final_path->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(last_win32_error("GetFileAttributesW"));
    }
    Root root{
        .final_path = std::move(*final_path),
        .is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        .read_only = read_only,
        .recursive = recursive,
        .label = std::move(label),
    };
    std::string display = core::wide_to_utf8(display_form(root.final_path));
    std::unique_lock lock(m_mutex);
    const auto existing = std::ranges::find_if(m_roots, [&root](const Root& r) { return iequals_wide(r.final_path, root.final_path); });
    if (existing != m_roots.end()) {
        *existing = std::move(root);
    } else {
        m_roots.push_back(std::move(root));
    }
    return display;
}

SecurityResult<std::string> FsScope::allow_base_dir(FsBaseDir base, std::string_view app_name, bool read_only) {
    auto dir = base_dir_path(base, app_name);
    if (!dir) {
        return std::unexpected(std::move(dir.error()));
    }
    static constexpr std::array<std::string_view, 9> kLabels = {
        "app_data", "app_local_data", "documents", "downloads", "desktop", "pictures", "music", "videos", "temp"};
    return allow(std::wstring_view(*dir), read_only, true, std::string(kLabels[static_cast<std::size_t>(base)]));
}

bool FsScope::revoke(std::string_view utf8_path) {
    auto normalized = normalize_lexically(core::utf8_to_wide(utf8_path));
    if (!normalized) {
        return false;
    }
    auto final_path = final_path_of(*normalized);
    const std::wstring target = final_path ? std::move(*final_path) : to_extended(*normalized);
    std::unique_lock lock(m_mutex);
    const auto removed = std::erase_if(m_roots, [&target](const Root& r) { return iequals_wide(r.final_path, target); });
    return removed != 0;
}

void FsScope::clear() {
    std::unique_lock lock(m_mutex);
    m_roots.clear();
}

std::vector<FsScopeInfo> FsScope::roots() const {
    std::shared_lock lock(m_mutex);
    std::vector<FsScopeInfo> out;
    out.reserve(m_roots.size());
    for (const auto& root : m_roots) {
        out.push_back(FsScopeInfo{
            .path = core::wide_to_utf8(display_form(root.final_path)),
            .label = root.label,
            .read_only = root.read_only,
            .recursive = root.recursive,
        });
    }
    return out;
}

bool FsScope::empty() const {
    std::shared_lock lock(m_mutex);
    return m_roots.empty();
}

// ---- Resolution ------------------------------------------------------------------------------------------

SecurityResult<std::wstring> FsScope::absolutize(std::string_view utf8_path) const {
    if (utf8_path.empty() || !is_valid_utf8(utf8_path)) {
        return std::unexpected(invalid_name("empty or not UTF-8"));
    }
    std::wstring wide = core::utf8_to_wide(utf8_path);
    const bool absolute_like = wide.size() >= 2 && is_drive_letter(wide[0]) && wide[1] == L':';
    const bool rooted = !wide.empty() && (wide[0] == L'\\' || wide[0] == L'/');
    if (!absolute_like && !rooted) {
        std::wstring base;
        {
            std::shared_lock lock(m_mutex);
            const auto dir = std::ranges::find_if(m_roots, &Root::is_directory);
            if (dir == m_roots.end()) {
                return std::unexpected(denied("relative path and no directory scope"));
            }
            base = display_form(dir->final_path);
        }
        if (base.starts_with(L"\\\\")) {
            return std::unexpected(denied("relative paths are not supported for network scopes"));
        }
        if (!base.ends_with(L'\\')) {
            base.push_back(L'\\');
        }
        wide = base + wide;
    }
    return normalize_lexically(wide);
}

SecurityResult<ResolvedPath> FsScope::check_contained(std::wstring final_path, FsAccess access) const {
    std::shared_lock lock(m_mutex);
    const Root* best = nullptr;
    bool is_root = false;
    for (const auto& root : m_roots) {
        const std::wstring_view rp = root.final_path;
        bool match = false;
        bool equal = false;
        if (iequals_wide(final_path, rp)) {
            match = true;
            equal = true;
        } else if (root.is_directory && final_path.size() > rp.size() + 1 && final_path[rp.size()] == L'\\' &&
                   iequals_wide(std::wstring_view(final_path).substr(0, rp.size()), rp)) {
            const std::wstring_view rest = std::wstring_view(final_path).substr(rp.size() + 1);
            match = root.recursive || rest.find(L'\\') == std::wstring_view::npos;
        }
        if (!match) {
            continue;
        }
        if (best == nullptr || (best->read_only && !root.read_only)) {
            best = &root;
            is_root = equal;
        }
    }
    if (best == nullptr) {
        return std::unexpected(denied("path is outside the allowed file-system scopes"));
    }
    if (access == FsAccess::Write && best->read_only) {
        return std::unexpected(denied("the file-system scope is read-only"));
    }
    ResolvedPath out{
        .win32 = {},
        .display = core::wide_to_utf8(display_form(final_path)),
        .lexical = {},
        .is_root = is_root,
        .read_only = best->read_only,
        .scope_recursive = best->recursive,
    };
    out.win32 = std::move(final_path);
    return out;
}

SecurityResult<ResolvedPath> FsScope::resolve(std::string_view utf8_path, FsAccess access) const {
    auto normalized = absolutize(utf8_path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    auto final_path = resolve_following(*normalized);
    if (!final_path) {
        return std::unexpected(std::move(final_path.error()));
    }
    auto resolved = check_contained(std::move(*final_path), access);
    if (resolved) {
        resolved->lexical = std::move(*normalized);
    }
    return resolved;
}

SecurityResult<ResolvedPath> FsScope::resolve_entry(std::string_view utf8_path, FsAccess access) const {
    auto normalized = absolutize(utf8_path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    std::wstring_view parent;
    std::wstring_view leaf;
    if (!split_last(*normalized, parent, leaf)) {
        return std::unexpected(denied("a drive root is not an entry"));
    }
    auto parent_final = resolve_following(parent);
    if (!parent_final) {
        return std::unexpected(std::move(parent_final.error()));
    }
    std::wstring full = std::move(*parent_final);
    full.push_back(L'\\');
    full.append(leaf);
    auto resolved = check_contained(std::move(full), access);
    if (resolved) {
        resolved->lexical = std::move(*normalized);
    }
    return resolved;
}

SecurityResult<void> FsScope::verify_handle(void* handle, FsAccess access) const {
    auto final_path = final_path_of_handle(handle);
    if (!final_path) {
        return std::unexpected(std::move(final_path.error()));
    }
    auto resolved = check_contained(std::move(*final_path), access);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    return {};
}

// ---- DirectoryWatcher --------------------------------------------------------------------------------------

struct DirectoryWatcher::Impl {
    static constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                     FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                     FILE_NOTIFY_CHANGE_CREATION;
    static constexpr std::size_t kBufferSize = 64 * 1024; // network shares cap at 64 KiB

    UniqueHandle directory;
    UniqueHandle idle_event;
    PTP_IO io = nullptr;
    OVERLAPPED overlapped{};
    alignas(8) std::array<std::byte, kBufferSize> buffer{};
    bool recursive = true;
    std::uint64_t id = 0;
    std::wstring display_dir;
    Callback callback;
    std::atomic<bool> stopping{false};

    bool issue() noexcept {
        ResetEvent(idle_event.get());
        StartThreadpoolIo(io);
        overlapped = OVERLAPPED{};
        if (!ReadDirectoryChangesW(directory.get(), buffer.data(), static_cast<DWORD>(buffer.size()), recursive ? TRUE : FALSE,
                                   kFilter, nullptr, &overlapped, nullptr)) {
            CancelThreadpoolIo(io);
            SetEvent(idle_event.get());
            return false;
        }
        return true;
    }

    void deliver(FsChangeBatch batch) noexcept {
        if (!callback) {
            return;
        }
        try {
            callback(std::move(batch));
        } catch (...) {
            core::hr_check(E_UNEXPECTED, "DirectoryWatcher: change callback threw");
        }
    }

    FsChangeBatch parse(ULONG_PTR bytes) const {
        FsChangeBatch batch{.id = id};
        std::size_t offset = 0;
        const std::size_t limit = std::min<std::size_t>(bytes, buffer.size());
        while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= limit) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            const std::size_t name_bytes = info->FileNameLength;
            if (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) + name_bytes > limit) {
                break;
            }
            const std::wstring_view name(info->FileName, name_bytes / sizeof(wchar_t));
            std::string_view kind;
            switch (info->Action) {
            case FILE_ACTION_ADDED:
                kind = "added";
                break;
            case FILE_ACTION_REMOVED:
                kind = "removed";
                break;
            case FILE_ACTION_MODIFIED:
                kind = "modified";
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                kind = "renamed_from";
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                kind = "renamed_to";
                break;
            default:
                kind = "modified";
                break;
            }
            std::wstring full = display_dir;
            if (!full.ends_with(L'\\')) {
                full.push_back(L'\\');
            }
            full.append(name);
            FsChange change{.kind = std::string(kind), .path = core::wide_to_utf8(full)};
            const bool duplicate = !batch.changes.empty() && batch.changes.back().kind == change.kind &&
                                   batch.changes.back().path == change.path;
            if (!duplicate) {
                batch.changes.push_back(std::move(change));
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            offset += info->NextEntryOffset;
        }
        return batch;
    }

    static void CALLBACK on_io(PTP_CALLBACK_INSTANCE, PVOID context, PVOID, ULONG result, ULONG_PTR bytes, PTP_IO) {
        auto* self = static_cast<Impl*>(context);
        if (result == ERROR_OPERATION_ABORTED || self->stopping.load(std::memory_order_acquire)) {
            SetEvent(self->idle_event.get());
            return;
        }
        if (result != NO_ERROR && result != ERROR_NOTIFY_ENUM_DIR) {
            self->deliver(FsChangeBatch{.id = self->id, .changes = {}, .overflow = false, .closed = true});
            SetEvent(self->idle_event.get());
            return;
        }
        try {
            if (bytes == 0 || result == ERROR_NOTIFY_ENUM_DIR) {
                self->deliver(FsChangeBatch{.id = self->id, .changes = {}, .overflow = true, .closed = false});
            } else {
                FsChangeBatch batch = self->parse(bytes);
                if (!batch.changes.empty()) {
                    self->deliver(std::move(batch));
                }
            }
        } catch (...) {
            core::hr_check(E_OUTOFMEMORY, "DirectoryWatcher: failed to build change batch");
        }
        if (self->stopping.load(std::memory_order_acquire) || !self->issue()) {
            if (!self->stopping.load(std::memory_order_acquire)) {
                self->deliver(FsChangeBatch{.id = self->id, .changes = {}, .overflow = false, .closed = true});
            }
            SetEvent(self->idle_event.get());
        }
    }
};

DirectoryWatcher::DirectoryWatcher(PassKey, std::unique_ptr<Impl> impl, std::wstring directory, std::uint64_t id)
    : m_impl(std::move(impl)), m_directory(std::move(directory)), m_id(id) {}

SecurityResult<std::unique_ptr<DirectoryWatcher>> DirectoryWatcher::start(
    std::wstring directory, bool recursive, std::uint64_t id, Callback callback) {
    auto impl = std::make_unique<Impl>();
    const std::wstring extended = directory.starts_with(kExtendedPrefix) ? directory : to_extended(directory);
    impl->directory.reset(CreateFileW(
        extended.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr));
    if (impl->directory.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(last_win32_error("CreateFileW (watch)"));
    }
    impl->idle_event.reset(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    if (impl->idle_event.get() == nullptr) {
        return std::unexpected(last_win32_error("CreateEventW"));
    }
    impl->recursive = recursive;
    impl->id = id;
    impl->display_dir = FsScope::display_form(directory);
    impl->callback = std::move(callback);
    impl->io = CreateThreadpoolIo(impl->directory.get(), &Impl::on_io, impl.get(), nullptr);
    if (impl->io == nullptr) {
        return std::unexpected(last_win32_error("CreateThreadpoolIo"));
    }
    if (!impl->issue()) {
        const SecurityError error = last_win32_error("ReadDirectoryChangesW");
        CloseThreadpoolIo(impl->io);
        impl->io = nullptr;
        return std::unexpected(error);
    }
    return std::make_unique<DirectoryWatcher>(PassKey{}, std::move(impl), std::move(directory), id);
}

DirectoryWatcher::~DirectoryWatcher() {
    if (!m_impl || m_impl->io == nullptr) {
        return;
    }
    m_impl->stopping.store(true, std::memory_order_release);
    // A completion that raced the flag may have re-issued a read; cancel until the watcher is idle.
    for (int attempt = 0; attempt < 100; ++attempt) {
        CancelIoEx(m_impl->directory.get(), nullptr);
        if (WaitForSingleObject(m_impl->idle_event.get(), 50) == WAIT_OBJECT_0) {
            break;
        }
    }
    WaitForThreadpoolIoCallbacks(m_impl->io, FALSE);
    CloseThreadpoolIo(m_impl->io);
    m_impl->io = nullptr;
}

} // namespace boson::security
