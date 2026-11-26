#pragma once

#include <boson/security/common.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace boson::fs {

using FsError = security::SecurityError;
template <typename T>
using FsResult = security::SecurityResult<T>;

/// @brief One planned change inside the install root.
struct InstallOperation {
    enum class Kind : std::uint8_t {
        Replace, ///< staging/<name> becomes root/<name> (an existing root/<name> is backed up first)
        Remove,  ///< root/<name> is removed (backed up first, deleted after commit)
    };
    Kind kind = Kind::Replace;
    std::filesystem::path name; ///< single path component (a folder or file directly under root)
};

struct CommitReport {
    std::size_t replaced = 0;     ///< targets that existed and were swapped
    std::size_t created = 0;      ///< targets that did not exist before
    std::size_t removed = 0;
    bool backups_deleted = true;  ///< false when a backup could not be deleted now (cleaned on next recover)
};

struct RecoveryReport {
    std::size_t transactions_found = 0;
    std::size_t rolled_back = 0;   ///< interrupted commits whose backups were restored
    std::size_t discarded = 0;     ///< abandoned staging areas / finished transactions deleted
    std::size_t skipped_live = 0;  ///< owned by a process that is still running
    std::vector<std::string> warnings;
};

/// @brief Staged, all-or-nothing replacement of entries directly under an install root (e.g. an AddOns
/// folder). Content is prepared in a hidden staging directory on the same volume
/// ("<root>/.boson-txn-<id>/staging"), then commit() swaps every target with two MoveFileExW renames
/// (target -> backup, staged -> target). A failure at any step renames everything back, in reverse order,
/// so the root is left exactly as it was. A journal written before the first rename lets recover() roll an
/// interrupted commit back after a crash or power loss. Blocking file I/O: use it from a pool thread.
class InstallTransaction {
public:
    /// @brief Creates the transaction directory under root (root is created if absent).
    [[nodiscard]] static FsResult<InstallTransaction> begin(const std::filesystem::path& root);

    InstallTransaction(InstallTransaction&&) noexcept;
    InstallTransaction& operator=(InstallTransaction&&) noexcept;
    InstallTransaction(const InstallTransaction&) = delete;
    InstallTransaction& operator=(const InstallTransaction&) = delete;
    /// @brief Discards the staging area unless commit() succeeded.
    ~InstallTransaction();

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    /// @brief Where new content goes: write / extract into staging_dir() / name.
    [[nodiscard]] const std::filesystem::path& staging_dir() const noexcept;
    [[nodiscard]] const std::filesystem::path& transaction_dir() const noexcept;
    [[nodiscard]] const std::vector<InstallOperation>& operations() const noexcept;

    /// @brief Plans root/name := staging/name. name must be one plain component and exist in staging.
    FsResult<void> replace(const std::filesystem::path& name);
    /// @brief Plans every top-level entry of the staging directory as a replace().
    FsResult<std::size_t> replace_all_staged();
    /// @brief Plans removal of root/name (no-op at commit when it does not exist).
    FsResult<void> remove(const std::filesystem::path& name);

    /// @brief Applies every planned operation atomically (see class comment). After success or failure the
    /// transaction is finished; a failed commit leaves the root untouched and returns the error.
    [[nodiscard]] FsResult<CommitReport> commit();
    /// @brief Deletes the staging area without touching the root.
    void abort() noexcept;
    [[nodiscard]] bool committed() const noexcept;

    /// @brief Test hook called before every rename of commit() with the number of renames already done;
    /// returning false injects a failure there (commit then rolls back). Tests also use it to kill the
    /// process mid-commit and exercise recover().
    void set_rename_hook_for_testing(std::function<bool(int renames_done)> hook);

    /// @brief Startup cleanup for root: rolls back commits interrupted by a crash (journal state
    /// "committing"), and deletes abandoned staging areas and leftover backups. Transactions owned by a
    /// process that is still alive are left alone.
    [[nodiscard]] static RecoveryReport recover(const std::filesystem::path& root);

private:
    struct Impl;
    explicit InstallTransaction(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

/// @brief Replaces target with staged (a directory or file on the same volume) using a backup + rename,
/// rolling back on failure. Convenience for single-target installs.
[[nodiscard]] FsResult<void> atomic_replace(const std::filesystem::path& staged, const std::filesystem::path& target);

/// @brief Recursively deletes a directory tree, clearing read-only attributes and never following
/// junctions / symlinks (they are unlinked, not traversed). Missing paths succeed.
FsResult<void> remove_tree(const std::filesystem::path& path) noexcept;

} // namespace boson::fs
