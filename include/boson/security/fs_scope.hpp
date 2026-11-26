#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boson/security/common.hpp>

namespace boson::security {

enum class FsAccess : std::uint8_t {
    Read,
    Write,
};

enum class FsBaseDir : std::uint8_t {
    AppData,      ///< %APPDATA%\<app>      (roaming; created on demand)
    AppLocalData, ///< %LOCALAPPDATA%\<app> (created on demand)
    Documents,
    Downloads,
    Desktop,
    Pictures,
    Music,
    Videos,
    Temp,         ///< GetTempPathW
};

/// @brief One allowed root as reported to callers (UTF-8, canonical DOS path).
struct FsScopeInfo {
    std::string path;
    std::string label;
    bool read_only = false;
    bool recursive = true;
};

/// @brief A path that passed every check. win32 is the extended-length form ("\\?\C:\..." or
/// "\\?\UNC\...") to hand to Win32 APIs; display is the plain form shown to the frontend.
struct ResolvedPath {
    std::wstring win32;
    std::string display;
    /// The lexically normalised input path (links not followed), e.g. for lstat-style attribute queries.
    std::wstring lexical;
    bool is_root = false;    ///< the path *is* a scope root (never removable / renamable)
    bool read_only = false;  ///< the matching scope is read-only
    bool scope_recursive = true; ///< the matching scope covers sub-directories
};

/// @brief Set of directories the frontend may touch. Thread-safe: roots can be granted while other threads
/// resolve paths. Every check is fail-closed: anything unusual is rejected with ERROR_ACCESS_DENIED or
/// ERROR_INVALID_NAME.
///
/// resolve() rejects, before touching the disk: UNC ("\\server"), device and Win32 namespace paths
/// ("\\.\", "\\?\", "\??\"), drive-relative paths ("C:foo"), alternate data streams ("a.txt:x"), reserved
/// device names (CON, NUL, COM1, LPT1, CONIN$ ... with or without extension), components with trailing dots
/// or spaces, wildcard / control characters and ".." that would climb above a drive root. It then collapses
/// "." and "..", opens the deepest existing ancestor and takes its final path (GetFinalPathNameByHandleW),
/// so symlinks and junctions are followed to their real target before the containment check.
class FsScope {
public:
    FsScope() = default;

    /// @brief Adds root (must exist; file or directory). Returns its canonical display path.
    SecurityResult<std::string> allow(std::string_view utf8_path, bool read_only = false, bool recursive = true, std::string label = {});
    SecurityResult<std::string> allow(std::wstring_view path, bool read_only = false, bool recursive = true, std::string label = {});
    /// @brief Adds a well-known folder. AppData/AppLocalData use app_name as sub-folder and create it.
    SecurityResult<std::string> allow_base_dir(FsBaseDir base, std::string_view app_name = {}, bool read_only = false);
    /// @brief Removes a root; true when it existed.
    bool revoke(std::string_view utf8_path);
    void clear();

    [[nodiscard]] std::vector<FsScopeInfo> roots() const;
    [[nodiscard]] bool empty() const;

    /// @brief Full check for opening the object at path (symlinks followed to their target).
    /// Relative paths are resolved against the first directory root.
    [[nodiscard]] SecurityResult<ResolvedPath> resolve(std::string_view utf8_path, FsAccess access) const;
    /// @brief Like resolve(), but the last component is not followed: the parent directory is resolved and
    /// the leaf name appended. Use it for remove / rename / lstat, which act on a link itself.
    [[nodiscard]] SecurityResult<ResolvedPath> resolve_entry(std::string_view utf8_path, FsAccess access) const;
    /// @brief Re-checks an already opened handle (closes the TOCTOU window between resolve() and open).
    [[nodiscard]] SecurityResult<void> verify_handle(void* handle, FsAccess access) const;

    /// @brief Lexical validation + normalisation only (no disk access): the absolute, "."/".."-free path
    /// with backslashes, or an error. Exposed for tests.
    [[nodiscard]] static SecurityResult<std::wstring> normalize_lexically(std::wstring_view path);
    [[nodiscard]] static bool is_reserved_device_name(std::wstring_view component) noexcept;
    /// @brief Final path of an existing object ("\\?\C:\..."), or error.
    [[nodiscard]] static SecurityResult<std::wstring> final_path_of(std::wstring_view path);
    [[nodiscard]] static SecurityResult<std::wstring> base_dir_path(FsBaseDir base, std::string_view app_name = {});
    /// @brief Strips the "\\?\" prefix ("\\?\UNC\x" -> "\\x").
    [[nodiscard]] static std::wstring display_form(std::wstring_view final_path);

private:
    struct Root {
        std::wstring final_path; ///< "\\?\C:\..." without trailing separator
        bool is_directory = true;
        bool read_only = false;
        bool recursive = true;
        std::string label;
    };

    SecurityResult<ResolvedPath> check_contained(std::wstring final_path, FsAccess access) const;
    SecurityResult<std::wstring> absolutize(std::string_view utf8_path) const;

    mutable std::shared_mutex m_mutex;
    std::vector<Root> m_roots;
};

// ---- Directory watching ----------------------------------------------------------------------------------

struct FsChange {
    std::string kind; ///< "added" | "removed" | "modified" | "renamed_from" | "renamed_to"
    std::string path;
};

struct FsChangeBatch {
    std::uint64_t id = 0;
    std::vector<FsChange> changes;
    /// The kernel buffer overflowed: some changes were lost, rescan the directory.
    bool overflow = false;
    /// The watch ended (directory deleted or unreachable); no more batches follow.
    bool closed = false;
};

/// @brief ReadDirectoryChangesW driven by the Win32 thread pool (overlapped I/O + CreateThreadpoolIo):
/// no dedicated thread and nothing ever blocks the UI thread. The callback runs on a thread-pool thread.
/// Destroying the watcher cancels the I/O and waits for a running callback; do not destroy it from inside
/// its own callback.
class DirectoryWatcher {
public:
    using Callback = std::function<void(FsChangeBatch)>;

    [[nodiscard]] static SecurityResult<std::unique_ptr<DirectoryWatcher>> start(
        std::wstring directory, bool recursive, std::uint64_t id, Callback callback);

    ~DirectoryWatcher();
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    [[nodiscard]] const std::wstring& directory() const noexcept { return m_directory; }

    struct Impl;
    /// Construction token: only start() can create watchers.
    class PassKey {
        friend class DirectoryWatcher;
        PassKey() = default;
    };
    DirectoryWatcher(PassKey, std::unique_ptr<Impl> impl, std::wstring directory, std::uint64_t id);

private:

    std::unique_ptr<Impl> m_impl;
    std::wstring m_directory;
    std::uint64_t m_id = 0;
};

} // namespace boson::security
