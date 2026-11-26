#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boson/security/common.hpp>
#include <boson/security/fs_scope.hpp>

namespace boson::security {

struct FsStat {
    bool exists = false;
    bool is_file = false;
    bool is_dir = false;
    bool is_symlink = false; ///< the path itself is a symlink / junction / other reparse point
    bool readonly = false;
    std::uint64_t size = 0;
    std::int64_t modified_ms = 0; ///< Unix epoch milliseconds
    std::int64_t created_ms = 0;
    std::int64_t accessed_ms = 0;
    std::string path; ///< canonical path (links resolved)
};

struct FsEntry {
    std::string name;
    std::string path;
    bool is_file = false;
    bool is_dir = false;
    bool is_symlink = false;
    std::uint64_t size = 0;
    std::int64_t modified_ms = 0;
};

/// @brief File operations confined to an FsScope. Every call resolves and checks its path(s) first, and
/// read/write/append re-check the opened handle's final path. Blocking: call it from the TaskRunner pool
/// (the IPC router already does), never from the UI thread. Thread-safe.
class ScopedFs {
public:
    static constexpr std::size_t kDefaultMaxReadBytes = 256ull * 1024 * 1024;
    static constexpr std::size_t kMaxWatches = 64;

    explicit ScopedFs(std::shared_ptr<FsScope> scope, std::size_t max_read_bytes = kDefaultMaxReadBytes);
    ~ScopedFs();
    ScopedFs(const ScopedFs&) = delete;
    ScopedFs& operator=(const ScopedFs&) = delete;

    [[nodiscard]] SecurityResult<std::vector<std::byte>> read_bytes(std::string_view path) const;
    /// @brief UTF-8 text (a UTF-8 BOM is stripped); ERROR_INVALID_DATA when the file is not UTF-8.
    [[nodiscard]] SecurityResult<std::string> read_text(std::string_view path) const;
    /// @brief Atomic replace: writes a temp file next to the target, then MoveFileExW(REPLACE_EXISTING).
    [[nodiscard]] SecurityResult<void> write_bytes(std::string_view path, std::span<const std::byte> data, bool create_dirs = false) const;
    [[nodiscard]] SecurityResult<void> write_text(std::string_view path, std::string_view text, bool create_dirs = false) const;
    [[nodiscard]] SecurityResult<void> append(std::string_view path, std::span<const std::byte> data, bool create_dirs = false) const;
    [[nodiscard]] SecurityResult<bool> exists(std::string_view path) const;
    [[nodiscard]] SecurityResult<FsStat> stat(std::string_view path) const;
    [[nodiscard]] SecurityResult<std::vector<FsEntry>> read_dir(std::string_view path) const;
    [[nodiscard]] SecurityResult<void> mkdir(std::string_view path, bool recursive = true) const;
    /// @brief Removes a file, an empty directory, or (recursive) a tree. Links are removed, never followed.
    /// Scope roots cannot be removed.
    [[nodiscard]] SecurityResult<void> remove(std::string_view path, bool recursive = false) const;
    [[nodiscard]] SecurityResult<void> rename(std::string_view from, std::string_view to, bool overwrite = false) const;
    /// @brief Copies a file (not directories).
    [[nodiscard]] SecurityResult<void> copy(std::string_view from, std::string_view to, bool overwrite = false) const;

    /// @brief Starts watching a directory; returns the watch id. callback runs on a Win32 thread-pool thread.
    [[nodiscard]] SecurityResult<std::uint64_t> watch(std::string_view path, bool recursive, DirectoryWatcher::Callback callback);
    bool unwatch(std::uint64_t id);
    void unwatch_all();
    [[nodiscard]] std::size_t watch_count() const;

    [[nodiscard]] FsScope& scope() noexcept { return *m_scope; }
    [[nodiscard]] const FsScope& scope() const noexcept { return *m_scope; }
    [[nodiscard]] const std::shared_ptr<FsScope>& shared_scope() const noexcept { return m_scope; }

private:
    SecurityResult<void> write_handle_all(void* handle, std::span<const std::byte> data) const;
    SecurityResult<void> ensure_parent(const ResolvedPath& target, bool create_dirs) const;

    std::shared_ptr<FsScope> m_scope;
    std::size_t m_max_read_bytes;
    mutable std::mutex m_watch_mutex;
    std::unordered_map<std::uint64_t, std::unique_ptr<DirectoryWatcher>> m_watches;
    std::uint64_t m_next_watch_id = 1;
};

} // namespace boson::security
