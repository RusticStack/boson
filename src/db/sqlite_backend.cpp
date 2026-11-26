#include <boson/db/database.hpp>

#include <boson/core/result.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <vector>

namespace boson::db {

namespace {

DbError make_error(sqlite3* db, int rc, std::string_view context = {}) {
    DbError err;
    err.code = rc & 0xFF;
    err.extended_code = db != nullptr ? sqlite3_extended_errcode(db) : rc;
    std::string msg(context);
    if (!msg.empty()) {
        msg += ": ";
    }
    msg += db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    err.message = std::move(msg);
    return err;
}

class SqliteStatement final : public detail::StatementImpl {
public:
    SqliteStatement(sqlite3_stmt* stmt, sqlite3* db) : m_stmt(stmt), m_db(db) {
        // "$1".."$N" are named parameters in SQLite; map the number to SQLite's index so SQL written for
        // both backends binds the same way. Plain "?" / "?N" keep their own index.
        const int count = sqlite3_bind_parameter_count(stmt);
        for (int i = 1; i <= count; ++i) {
            const char* name = sqlite3_bind_parameter_name(stmt, i);
            if (name == nullptr || name[0] != '$') {
                continue;
            }
            int n = 0;
            bool digits = name[1] != '\0';
            for (const char* p = name + 1; *p != '\0'; ++p) {
                digits = digits && *p >= '0' && *p <= '9';
                n = n * 10 + (*p - '0');
            }
            if (digits && n > 0 && n < 4096) {
                if (m_map.size() <= static_cast<std::size_t>(n)) {
                    m_map.resize(static_cast<std::size_t>(n) + 1, 0);
                }
                m_map[static_cast<std::size_t>(n)] = i;
            }
        }
    }
    ~SqliteStatement() override { sqlite3_finalize(m_stmt); }
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    DbResult<void> bind_int(int index, std::int64_t value) noexcept override { return check(sqlite3_bind_int64(m_stmt, map(index), value)); }
    DbResult<void> bind_bool(int index, bool value) noexcept override { return check(sqlite3_bind_int64(m_stmt, map(index), value ? 1 : 0)); }
    DbResult<void> bind_double(int index, double value) noexcept override { return check(sqlite3_bind_double(m_stmt, map(index), value)); }
    DbResult<void> bind_text(int index, std::string_view text) noexcept override {
        return check(sqlite3_bind_text64(m_stmt, map(index), text.data() == nullptr ? "" : text.data(),
                                         static_cast<sqlite3_uint64>(text.size()), SQLITE_TRANSIENT, SQLITE_UTF8));
    }
    DbResult<void> bind_blob(int index, std::span<const std::byte> blob) noexcept override {
        return check(sqlite3_bind_blob64(m_stmt, map(index), blob.empty() ? "" : static_cast<const void*>(blob.data()),
                                         static_cast<sqlite3_uint64>(blob.size()), SQLITE_TRANSIENT));
    }
    DbResult<void> bind_null(int index) noexcept override { return check(sqlite3_bind_null(m_stmt, map(index))); }
    int parameter_count() const noexcept override {
        return m_map.empty() ? sqlite3_bind_parameter_count(m_stmt) : static_cast<int>(m_map.size()) - 1;
    }

    DbResult<bool> step() noexcept override {
        const int rc = sqlite3_step(m_stmt);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        DbError err = make_error(m_db, rc, sql());
        sqlite3_reset(m_stmt);
        return std::unexpected(std::move(err));
    }

    DbResult<std::int64_t> execute() noexcept override {
        while (true) {
            auto r = step();
            if (!r) {
                return std::unexpected(std::move(r.error()));
            }
            if (!*r) {
                break;
            }
        }
        const std::int64_t changed = sqlite3_changes64(m_db);
        sqlite3_reset(m_stmt);
        return changed;
    }

    void reset() noexcept override {
        sqlite3_reset(m_stmt);
        sqlite3_clear_bindings(m_stmt);
    }

    int column_count() const noexcept override { return sqlite3_column_count(m_stmt); }
    ColumnType column_type(int column) const noexcept override { return static_cast<ColumnType>(sqlite3_column_type(m_stmt, column)); }
    std::int64_t column_int64(int column) const noexcept override { return sqlite3_column_int64(m_stmt, column); }
    double column_double(int column) const noexcept override { return sqlite3_column_double(m_stmt, column); }
    std::string_view column_text(int column) const noexcept override {
        const auto* text = sqlite3_column_text(m_stmt, column);
        const int bytes = sqlite3_column_bytes(m_stmt, column);
        return text == nullptr ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
    }
    std::span<const std::byte> column_blob(int column) const noexcept override {
        const void* data = sqlite3_column_blob(m_stmt, column);
        const int bytes = sqlite3_column_bytes(m_stmt, column);
        return data == nullptr ? std::span<const std::byte>{} : std::span<const std::byte>(static_cast<const std::byte*>(data), static_cast<std::size_t>(bytes));
    }
    std::string_view column_name(int column) const noexcept override {
        const char* name = sqlite3_column_name(m_stmt, column);
        return name == nullptr ? std::string_view{} : std::string_view(name);
    }
    std::string_view sql() const noexcept override {
        const char* text = sqlite3_sql(m_stmt);
        return text == nullptr ? std::string_view{} : std::string_view(text);
    }
    void* native_handle() const noexcept override { return m_stmt; }

private:
    int map(int index) const noexcept {
        if (!m_map.empty() && index > 0 && static_cast<std::size_t>(index) < m_map.size() && m_map[static_cast<std::size_t>(index)] != 0) {
            return m_map[static_cast<std::size_t>(index)];
        }
        return index;
    }
    DbResult<void> check(int rc) const {
        if (rc != SQLITE_OK) {
            return std::unexpected(make_error(m_db, rc, sql()));
        }
        return {};
    }

    sqlite3_stmt* m_stmt;
    sqlite3* m_db;
    std::vector<int> m_map;
};

class SqliteConnection final : public detail::ConnectionImpl {
public:
    explicit SqliteConnection(sqlite3* db) noexcept : m_db(db) {}
    ~SqliteConnection() override { sqlite3_close_v2(m_db); }
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    Backend backend() const noexcept override { return Backend::Sqlite; }

    DbResult<void> exec(std::string_view sql) override {
        const std::string text(sql);
        char* message = nullptr;
        const int rc = sqlite3_exec(m_db, text.c_str(), nullptr, nullptr, &message);
        if (rc != SQLITE_OK) {
            DbError err{.code = rc & 0xFF, .extended_code = sqlite3_extended_errcode(m_db), .message = message != nullptr ? message : sqlite3_errstr(rc)};
            sqlite3_free(message);
            return std::unexpected(std::move(err));
        }
        return {};
    }

    DbResult<std::unique_ptr<detail::StatementImpl>> prepare(std::string_view sql, bool persistent) override {
        sqlite3_stmt* stmt = nullptr;
        const int rc = sqlite3_prepare_v3(m_db, sql.data(), static_cast<int>(sql.size()), persistent ? SQLITE_PREPARE_PERSISTENT : 0, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return std::unexpected(make_error(m_db, rc, sql));
        }
        if (stmt == nullptr) {
            return std::unexpected(DbError{.code = SQLITE_MISUSE, .extended_code = SQLITE_MISUSE, .message = "empty SQL statement"});
        }
        return std::make_unique<SqliteStatement>(stmt, m_db);
    }

    DbResult<void> begin() override { return exec("BEGIN IMMEDIATE"); }
    DbResult<void> commit() override { return exec("COMMIT"); }
    DbResult<void> rollback() override { return exec("ROLLBACK"); }
    DbResult<void> savepoint(int depth) override { return exec("SAVEPOINT boson_sp_" + std::to_string(depth)); }
    DbResult<void> release_savepoint(int depth, bool commit) override {
        const std::string name = "boson_sp_" + std::to_string(depth);
        if (commit) {
            return exec("RELEASE " + name);
        }
        auto r = exec("ROLLBACK TO " + name);
        (void)exec("RELEASE " + name);
        return r;
    }
    std::int64_t last_insert_rowid() const noexcept override { return sqlite3_last_insert_rowid(m_db); }
    std::int64_t changes() const noexcept override { return sqlite3_changes64(m_db); }
    void* native_handle() const noexcept override { return m_db; }

    DbResult<void> checkpoint() override {
        const int rc = sqlite3_wal_checkpoint_v2(m_db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
        if (rc != SQLITE_OK) {
            return std::unexpected(make_error(m_db, rc, "checkpoint"));
        }
        return {};
    }

    DbResult<bool> integrity_check() override {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "PRAGMA integrity_check", -1, &stmt, nullptr) != SQLITE_OK) {
            return std::unexpected(make_error(m_db, sqlite3_errcode(m_db), "integrity_check"));
        }
        bool ok = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(stmt, 0);
            ok = text != nullptr && std::string_view(reinterpret_cast<const char*>(text)) == "ok";
        }
        sqlite3_finalize(stmt);
        return ok;
    }

    DbResult<void> backup_to(const std::filesystem::path& target) override {
        sqlite3* dest = nullptr;
        const std::string path = core::wide_to_utf8(target.native());
        int rc = sqlite3_open_v2(path.c_str(), &dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            DbError err = make_error(dest, rc, "open backup target");
            sqlite3_close_v2(dest);
            return std::unexpected(std::move(err));
        }
        sqlite3_backup* backup = sqlite3_backup_init(dest, "main", m_db, "main");
        if (backup == nullptr) {
            DbError err = make_error(dest, sqlite3_errcode(dest), "backup init");
            sqlite3_close_v2(dest);
            return std::unexpected(std::move(err));
        }
        rc = sqlite3_backup_step(backup, -1);
        sqlite3_backup_finish(backup);
        DbResult<void> out{};
        if (rc != SQLITE_DONE) {
            out = std::unexpected(make_error(dest, rc, "backup"));
        }
        sqlite3_close_v2(dest);
        return out;
    }

private:
    sqlite3* m_db;
};

} // namespace

namespace detail {

std::string_view sqlite_library_version() noexcept { return sqlite3_libversion(); }

DbResult<std::unique_ptr<ConnectionImpl>> open_sqlite(const DatabaseOptions& options) {
    if (!options.encryption_key.empty()) {
        return std::unexpected(DbError{.code = SQLITE_MISUSE, .extended_code = SQLITE_MISUSE,
                                       .message = "the SQLite backend has no encryption at rest (use Backend::Zephyr or SafeStorage)"});
    }
    std::string path;
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    if (options.path.empty()) {
        path = ":memory:";
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY;
    } else {
        if (!options.read_only && options.create && options.path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(options.path.parent_path(), ec);
        }
        path = core::wide_to_utf8(options.path.native());
        flags |= options.read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
        if (!options.read_only && options.create) {
            flags |= SQLITE_OPEN_CREATE;
        }
    }
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        DbError err = make_error(raw, rc, "open " + path);
        sqlite3_close_v2(raw);
        return std::unexpected(std::move(err));
    }
    auto conn = std::make_unique<SqliteConnection>(raw);
    sqlite3_busy_timeout(raw, static_cast<int>(std::clamp<long long>(options.busy_timeout.count(), 0, 0x7FFFFFFF)));
    sqlite3_extended_result_codes(raw, 1);

    std::string pragmas;
    if (options.wal && !options.path.empty() && !options.read_only) {
        pragmas += "PRAGMA journal_mode=WAL;";
    }
    static constexpr std::string_view kSync[] = {"OFF", "NORMAL", "FULL", "EXTRA"};
    pragmas += "PRAGMA synchronous=" + std::string(kSync[static_cast<int>(options.synchronous)]) + ";";
    pragmas += options.foreign_keys ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;";
    pragmas += "PRAGMA temp_store=MEMORY;";
    pragmas += "PRAGMA cache_size=-" + std::to_string(options.cache_kib) + ";";
    if (!options.path.empty()) {
        pragmas += "PRAGMA mmap_size=" + std::to_string(options.mmap_bytes) + ";";
    }
    if (auto r = conn->exec(pragmas); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return std::unique_ptr<ConnectionImpl>(std::move(conn));
}

} // namespace detail

} // namespace boson::db
