#include <boson/fs/install_transaction.hpp>

#include <boson/core/result.hpp>
#include <boson/fs/known_folders.hpp>
#include <boson/shipping/crypto.hpp>

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <utility>

namespace boson::fs {

namespace detail {

struct JournalOp {
    std::string kind; ///< "replace" | "remove"
    std::string name;
    bool had_target = false;
};

struct Journal {
    std::string state; ///< "staging" | "committing" | "committed"
    std::uint32_t pid = 0;
    std::uint64_t process_start = 0;
    std::vector<JournalOp> ops;
};

} // namespace detail

namespace {

using detail::Journal;
using detail::JournalOp;

constexpr std::wstring_view kTxnPrefix = L".boson-txn-";

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::wstring extended(const std::filesystem::path& p) {
    const std::wstring& s = p.native();
    if (s.starts_with(L"\\\\?\\")) {
        return s;
    }
    if (s.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return L"\\\\?\\" + s;
}

FsError win32_error(std::string_view context, DWORD code = GetLastError()) {
    return security::make_win32_error(code, context);
}

std::uint64_t process_start_time(DWORD pid) noexcept {
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return 0;
    }
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool exists_no_follow(const std::filesystem::path& p) noexcept {
    return GetFileAttributesW(extended(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// MoveFileExW without replace, retried briefly: antivirus / indexer handles make directory renames fail
/// with ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION for a few milliseconds after files were written.
bool rename_path(const std::filesystem::path& from, const std::filesystem::path& to, DWORD& error) noexcept {
    const std::wstring src = extended(from);
    const std::wstring dst = extended(to);
    DWORD delay = 10;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
            return false;
        }
        Sleep(delay);
        delay *= 2;
    }
    return false;
}

FsResult<void> write_journal(const std::filesystem::path& dir, const Journal& journal) {
    std::string text;
    if (glz::write_json(journal, text)) {
        return std::unexpected(FsError{.code = ERROR_INVALID_DATA, .message = "cannot serialize transaction journal"});
    }
    const std::filesystem::path final_path = dir / L"journal.json";
    const std::filesystem::path tmp = dir / L"journal.json.tmp";
    {
        const UniqueHandle file(CreateFileW(extended(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) {
            return std::unexpected(win32_error("cannot write transaction journal"));
        }
        DWORD written = 0;
        if (!WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size()) {
            return std::unexpected(win32_error("cannot write transaction journal"));
        }
        FlushFileBuffers(file.get());
    }
    if (!MoveFileExW(extended(tmp).c_str(), extended(final_path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return std::unexpected(win32_error("cannot commit transaction journal"));
    }
    return {};
}

std::optional<Journal> read_journal(const std::filesystem::path& dir) {
    std::ifstream in(dir / L"journal.json", std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Journal journal;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(journal, text)) {
        return std::nullopt;
    }
    return journal;
}

bool plain_component(const std::filesystem::path& name) {
    if (name.empty() || name.has_root_name() || name.has_root_directory() || name.has_parent_path()) {
        return false;
    }
    const std::wstring& s = name.native();
    return s != L"." && s != L".." && s.find_first_of(L"/\\:") == std::wstring::npos;
}

std::string random_suffix() {
    const auto bytes = shipping::random_bytes(8);
    return bytes ? shipping::to_hex(*bytes) : std::to_string(GetTickCount64());
}

} // namespace

FsResult<void> remove_tree(const std::filesystem::path& path) noexcept {
    try {
        const std::wstring ext = extended(path);
        const DWORD attrs = GetFileAttributesW(ext.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return {};
            }
            return std::unexpected(win32_error("cannot stat " + path.string(), code));
        }
        if ((attrs & FILE_ATTRIBUTE_READONLY) != 0) {
            SetFileAttributesW(ext.c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
        }
        const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool is_link = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (!is_dir) {
            if (!DeleteFileW(ext.c_str())) {
                return std::unexpected(win32_error("cannot delete " + path.string()));
            }
            return {};
        }
        if (!is_link) {
            WIN32_FIND_DATAW data{};
            const std::wstring pattern = ext + L"\\*";
            HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find != INVALID_HANDLE_VALUE) {
                std::optional<FsError> failure;
                do {
                    const std::wstring_view name = data.cFileName;
                    if (name == L"." || name == L"..") {
                        continue;
                    }
                    if (auto r = remove_tree(path / std::wstring(name)); !r && !failure) {
                        failure = std::move(r.error());
                    }
                } while (FindNextFileW(find, &data));
                FindClose(find);
                if (failure) {
                    return std::unexpected(std::move(*failure));
                }
            }
        }
        // A junction / directory symlink is removed itself, never traversed.
        DWORD delay = 10;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (RemoveDirectoryW(ext.c_str())) {
                return {};
            }
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return {};
            }
            if (code != ERROR_DIR_NOT_EMPTY && code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED) {
                return std::unexpected(win32_error("cannot remove directory " + path.string(), code));
            }
            Sleep(delay);
            delay *= 2;
        }
        return std::unexpected(win32_error("cannot remove directory " + path.string()));
    } catch (...) {
        return std::unexpected(FsError{.code = ERROR_NOT_ENOUGH_MEMORY, .message = "remove_tree: out of memory"});
    }
}

struct InstallTransaction::Impl {
    std::filesystem::path root;
    std::filesystem::path dir;
    std::filesystem::path staging;
    std::filesystem::path backup;
    std::vector<InstallOperation> ops;
    Journal journal;
    bool finished = false;
    bool committed = false;
    std::function<bool(int)> hook;

    void discard() noexcept {
        if (!finished) {
            finished = true;
            (void)remove_tree(dir);
        }
    }
};

InstallTransaction::InstallTransaction(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
InstallTransaction::InstallTransaction(InstallTransaction&&) noexcept = default;
InstallTransaction& InstallTransaction::operator=(InstallTransaction&& other) noexcept {
    if (this != &other) {
        if (m_impl) {
            m_impl->discard();
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

InstallTransaction::~InstallTransaction() {
    if (m_impl) {
        m_impl->discard();
    }
}

FsResult<InstallTransaction> InstallTransaction::begin(const std::filesystem::path& root) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(root, ec);
    if (ec) {
        return std::unexpected(FsError{.code = ERROR_BAD_PATHNAME, .message = "bad install root: " + ec.message()});
    }
    std::filesystem::create_directories(abs, ec);
    if (ec) {
        return std::unexpected(FsError{.code = static_cast<std::uint32_t>(ec.value()), .message = "cannot create install root: " + ec.message()});
    }
    auto impl = std::make_unique<Impl>();
    impl->root = abs;
    const DWORD pid = GetCurrentProcessId();
    impl->dir = abs / (std::wstring(kTxnPrefix) + std::to_wstring(pid) + L"-" + core::utf8_to_wide(random_suffix()));
    impl->staging = impl->dir / L"staging";
    impl->backup = impl->dir / L"backup";
    std::filesystem::create_directories(extended(impl->staging), ec);
    if (!ec) {
        std::filesystem::create_directories(extended(impl->backup), ec);
    }
    if (ec) {
        return std::unexpected(FsError{.code = static_cast<std::uint32_t>(ec.value()), .message = "cannot create staging area: " + ec.message()});
    }
    SetFileAttributesW(extended(impl->dir).c_str(), FILE_ATTRIBUTE_HIDDEN);
    impl->journal = Journal{.state = "staging", .pid = pid, .process_start = process_start_time(pid), .ops = {}};
    if (auto w = write_journal(impl->dir, impl->journal); !w) {
        (void)remove_tree(impl->dir);
        return std::unexpected(std::move(w.error()));
    }
    return InstallTransaction(std::move(impl));
}

const std::filesystem::path& InstallTransaction::root() const noexcept { return m_impl->root; }
const std::filesystem::path& InstallTransaction::staging_dir() const noexcept { return m_impl->staging; }
const std::filesystem::path& InstallTransaction::transaction_dir() const noexcept { return m_impl->dir; }
const std::vector<InstallOperation>& InstallTransaction::operations() const noexcept { return m_impl->ops; }
bool InstallTransaction::committed() const noexcept { return m_impl && m_impl->committed; }
void InstallTransaction::set_rename_hook_for_testing(std::function<bool(int)> hook) { m_impl->hook = std::move(hook); }

FsResult<void> InstallTransaction::replace(const std::filesystem::path& name) {
    if (m_impl->finished) {
        return std::unexpected(FsError{.code = ERROR_INVALID_STATE, .message = "transaction already finished"});
    }
    if (!plain_component(name)) {
        return std::unexpected(FsError{.code = ERROR_BAD_PATHNAME, .message = "install name must be a single path component"});
    }
    if (!exists_no_follow(m_impl->staging / name)) {
        return std::unexpected(FsError{.code = ERROR_FILE_NOT_FOUND, .message = "nothing staged for " + name.string()});
    }
    for (const auto& op : m_impl->ops) {
        if (CompareStringOrdinal(op.name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return std::unexpected(FsError{.code = ERROR_ALREADY_EXISTS, .message = "already planned: " + name.string()});
        }
    }
    m_impl->ops.push_back(InstallOperation{.kind = InstallOperation::Kind::Replace, .name = name});
    return {};
}

FsResult<std::size_t> InstallTransaction::replace_all_staged() {
    std::error_code ec;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(extended(m_impl->staging), ec)) {
        if (auto r = replace(entry.path().filename()); !r) {
            return std::unexpected(std::move(r.error()));
        }
        ++count;
    }
    if (ec) {
        return std::unexpected(FsError{.code = static_cast<std::uint32_t>(ec.value()), .message = "cannot list staging area: " + ec.message()});
    }
    return count;
}

FsResult<void> InstallTransaction::remove(const std::filesystem::path& name) {
    if (m_impl->finished) {
        return std::unexpected(FsError{.code = ERROR_INVALID_STATE, .message = "transaction already finished"});
    }
    if (!plain_component(name)) {
        return std::unexpected(FsError{.code = ERROR_BAD_PATHNAME, .message = "install name must be a single path component"});
    }
    m_impl->ops.push_back(InstallOperation{.kind = InstallOperation::Kind::Remove, .name = name});
    return {};
}

void InstallTransaction::abort() noexcept {
    if (m_impl) {
        m_impl->discard();
    }
}

FsResult<CommitReport> InstallTransaction::commit() {
    Impl& t = *m_impl;
    if (t.finished) {
        return std::unexpected(FsError{.code = ERROR_INVALID_STATE, .message = "transaction already finished"});
    }
    CommitReport report;
    t.journal.state = "committing";
    t.journal.ops.clear();
    std::vector<bool> had(t.ops.size(), false);
    for (std::size_t i = 0; i < t.ops.size(); ++i) {
        had[i] = exists_no_follow(t.root / t.ops[i].name);
        t.journal.ops.push_back(JournalOp{
            .kind = t.ops[i].kind == InstallOperation::Kind::Replace ? "replace" : "remove",
            .name = core::wide_to_utf8(t.ops[i].name.native()),
            .had_target = had[i],
        });
    }
    if (auto w = write_journal(t.dir, t.journal); !w) {
        t.discard();
        return std::unexpected(std::move(w.error()));
    }

    enum class StepKind : std::uint8_t { BackedUp, Placed };
    struct Step {
        std::size_t op;
        StepKind kind;
    };
    std::vector<Step> steps;
    steps.reserve(t.ops.size() * 2);
    int renames = 0;
    std::optional<FsError> failure;
    const auto do_rename = [&](const std::filesystem::path& from, const std::filesystem::path& to, std::string_view what) {
        if (t.hook && !t.hook(renames)) {
            failure = FsError{.code = ERROR_GEN_FAILURE, .message = "injected failure before " + std::string(what)};
            return false;
        }
        DWORD code = 0;
        if (!rename_path(from, to, code)) {
            failure = security::make_win32_error(code, std::string(what) + " " + to.string());
            return false;
        }
        ++renames;
        return true;
    };

    for (std::size_t i = 0; i < t.ops.size() && !failure; ++i) {
        const auto& op = t.ops[i];
        const std::filesystem::path target = t.root / op.name;
        if (had[i]) {
            if (!do_rename(target, t.backup / op.name, "backup of")) {
                break;
            }
            steps.push_back(Step{i, StepKind::BackedUp});
        }
        if (op.kind == InstallOperation::Kind::Replace) {
            if (!do_rename(t.staging / op.name, target, "install of")) {
                break;
            }
            steps.push_back(Step{i, StepKind::Placed});
        }
    }

    if (failure) {
        bool clean = true;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            const auto& op = t.ops[it->op];
            const std::filesystem::path target = t.root / op.name;
            DWORD code = 0;
            const bool ok = it->kind == StepKind::Placed ? rename_path(target, t.staging / op.name, code)
                                                         : rename_path(t.backup / op.name, target, code);
            clean = clean && ok;
        }
        if (clean) {
            t.discard();
        } else {
            // Leave the journal in "committing": recover() finishes the rollback on the next start.
            t.finished = true;
            failure->message += " (rollback incomplete; InstallTransaction::recover will finish it)";
        }
        return std::unexpected(std::move(*failure));
    }

    t.journal.state = "committed";
    (void)write_journal(t.dir, t.journal);
    for (std::size_t i = 0; i < t.ops.size(); ++i) {
        if (t.ops[i].kind == InstallOperation::Kind::Remove) {
            report.removed += had[i] ? 1 : 0;
        } else if (had[i]) {
            ++report.replaced;
        } else {
            ++report.created;
        }
    }
    t.committed = true;
    t.finished = true;
    report.backups_deleted = remove_tree(t.dir).has_value();
    return report;
}

RecoveryReport InstallTransaction::recover(const std::filesystem::path& root_in) {
    RecoveryReport report;
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(root_in, ec);
    if (ec) {
        report.warnings.push_back("bad install root: " + ec.message());
        return report;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const std::wstring name = entry.path().filename().native();
        if (!name.starts_with(kTxnPrefix) || !entry.is_directory(ec)) {
            continue;
        }
        ++report.transactions_found;
        const std::filesystem::path dir = entry.path();
        const auto journal = read_journal(dir);
        if (journal && journal->pid != 0 && process_alive(journal->pid) &&
            (journal->process_start == 0 || process_start_time(journal->pid) == journal->process_start)) {
            ++report.skipped_live;
            continue;
        }
        if (!journal) {
            std::error_code e2;
            const bool backups_present = !std::filesystem::is_empty(dir / L"backup", e2) && !e2;
            if (backups_present) {
                report.warnings.push_back("unreadable journal with backups left in place: " + dir.string());
                continue;
            }
        } else if (journal->state == "committing") {
            bool clean = true;
            for (auto it = journal->ops.rbegin(); it != journal->ops.rend(); ++it) {
                const std::filesystem::path op_name = core::utf8_to_wide(it->name);
                if (!plain_component(op_name)) {
                    clean = false;
                    continue;
                }
                const std::filesystem::path target = root / op_name;
                const std::filesystem::path staged = dir / L"staging" / op_name;
                const std::filesystem::path backup = dir / L"backup" / op_name;
                DWORD code = 0;
                if (it->kind == "replace" && !exists_no_follow(staged) && exists_no_follow(target)) {
                    clean = rename_path(target, staged, code) && clean;
                }
                if (exists_no_follow(backup)) {
                    if (exists_no_follow(target)) {
                        clean = remove_tree(target).has_value() && clean;
                    }
                    clean = rename_path(backup, target, code) && clean;
                }
            }
            if (!clean) {
                report.warnings.push_back("could not fully roll back " + dir.string());
                continue;
            }
            ++report.rolled_back;
        }
        if (remove_tree(dir)) {
            ++report.discarded;
        } else {
            report.warnings.push_back("cannot delete " + dir.string());
        }
    }
    return report;
}

FsResult<void> atomic_replace(const std::filesystem::path& staged_in, const std::filesystem::path& target_in) {
    std::error_code ec;
    const std::filesystem::path staged = std::filesystem::absolute(staged_in, ec);
    const std::filesystem::path target = std::filesystem::absolute(target_in, ec);
    if (ec) {
        return std::unexpected(FsError{.code = ERROR_BAD_PATHNAME, .message = "bad path: " + ec.message()});
    }
    if (!exists_no_follow(staged)) {
        return std::unexpected(FsError{.code = ERROR_FILE_NOT_FOUND, .message = "nothing staged at " + staged.string()});
    }
    std::filesystem::path backup = target;
    backup += core::utf8_to_wide(".boson-bak-" + random_suffix());
    const bool had = exists_no_follow(target);
    DWORD code = 0;
    if (had && !rename_path(target, backup, code)) {
        return std::unexpected(security::make_win32_error(code, "backup of " + target.string()));
    }
    if (!rename_path(staged, target, code)) {
        const DWORD install_error = code;
        if (had) {
            rename_path(backup, target, code);
        }
        return std::unexpected(security::make_win32_error(install_error, "install of " + target.string()));
    }
    if (had) {
        (void)remove_tree(backup);
    }
    return {};
}

} // namespace boson::fs
