#include <boson/db/database.hpp>

#include <boson/core/result.hpp>

#include <zephyr/c_abi.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <set>
#include <vector>

#ifndef BOSON_ZEPHYR_COMMIT
#define BOSON_ZEPHYR_COMMIT "unknown"
#endif

namespace boson::db {

namespace {

DbError zerror(ZephyrDB* db, int status, std::string_view context) {
    std::string msg(context);
    const char* detail = db != nullptr ? zephyr_last_error(db) : nullptr;
    if (detail != nullptr && detail[0] != '\0') {
        if (!msg.empty()) {
            msg += ": ";
        }
        msg += detail;
    } else {
        msg += " (ZephyrDB status " + std::to_string(status) + ")";
    }
    return DbError{.code = status, .extended_code = status, .message = std::move(msg)};
}

struct ResultFree {
    void operator()(ZephyrResultSet* r) const noexcept { zephyr_result_free(r); }
};
using UniqueResult = std::unique_ptr<ZephyrResultSet, ResultFree>;

struct StmtFree {
    void operator()(ZephyrStmt* s) const noexcept { zephyr_stmt_free(s); }
};

/// One engine per data directory in this process: two engines replaying the same WAL would corrupt it.
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
std::set<std::wstring>& registry() {
    static std::set<std::wstring> dirs;
    return dirs;
}

std::wstring dir_key(const std::filesystem::path& p) {
    std::error_code ec;
    std::wstring s = std::filesystem::weakly_canonical(p, ec).native();
    if (ec) {
        s = std::filesystem::absolute(p, ec).native();
    }
    if (!s.empty()) {
        CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
    }
    return s;
}

/// True for INSERT / UPDATE / DELETE (after leading whitespace and comments are skipped).
bool is_write_sql(std::string_view sql) noexcept {
    std::size_t i = 0;
    while (i < sql.size()) {
        if (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n') {
            ++i;
        } else if (sql.substr(i).starts_with("--")) {
            const auto nl = sql.find('\n', i);
            i = nl == std::string_view::npos ? sql.size() : nl + 1;
        } else {
            break;
        }
    }
    const auto word = sql.substr(i, 6);
    const auto ieq = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return (x >= 'a' && x <= 'z' ? static_cast<char>(x - 32) : x) == y;
               });
    };
    return ieq(word, "INSERT") || ieq(word, "UPDATE") || ieq(word, "DELETE");
}

class ZephyrConnection;

// ZephyrDB c50eea7 bug (worked around here): a parameterised INSERT / UPDATE / DELETE that does not take
// the engine's PK fast path (any statement inside BEGIN..COMMIT, text primary keys, multi-column SET,
// DELETE ... WHERE) is applied in memory but never written to the WAL (executor.cpp exec_stmt logs only
// when params.empty()), so it is lost on a crash or kill even after COMMIT returned. On a persistent
// database Boson therefore runs writes with their parameters inlined as SQL literals, which ZephyrDB
// logs verbatim. Reads keep using the prepared statement.
class ZephyrStatement final : public detail::StatementImpl {
public:
    ZephyrStatement(ZephyrConnection& conn, ZephyrDB* db, std::string sql, ZephyrStmt* stmt, bool persistent)
        : m_conn(&conn), m_db(db), m_sql(std::move(sql)), m_stmt(stmt), m_inline_params(persistent && is_write_sql(m_sql)) {}

    DbResult<void> bind_int(int index, std::int64_t value) noexcept override {
        return set(index, [&](Param& p) { p.type = ZEPHYR_TYPE_I64; p.i64 = value; });
    }
    DbResult<void> bind_bool(int index, bool value) noexcept override {
        return set(index, [&](Param& p) { p.type = ZEPHYR_TYPE_BOOL; p.i64 = value ? 1 : 0; });
    }
    DbResult<void> bind_double(int index, double value) noexcept override {
        return set(index, [&](Param& p) { p.type = ZEPHYR_TYPE_F64; p.f64 = value; });
    }
    DbResult<void> bind_text(int index, std::string_view text) noexcept override {
        return set(index, [&](Param& p) { p.type = ZEPHYR_TYPE_TEXT; p.bytes.assign(text.data(), text.size()); });
    }
    DbResult<void> bind_blob(int index, std::span<const std::byte> blob) noexcept override {
        return set(index, [&](Param& p) {
            p.type = ZEPHYR_TYPE_BYTEA;
            p.bytes.assign(reinterpret_cast<const char*>(blob.data()), blob.size());
        });
    }
    DbResult<void> bind_null(int index) noexcept override {
        return set(index, [&](Param& p) { p.type = ZEPHYR_TYPE_NULL; });
    }
    int parameter_count() const noexcept override { return static_cast<int>(m_params.size()); }

    DbResult<bool> step() noexcept override;
    DbResult<std::int64_t> execute() noexcept override;

    void reset() noexcept override {
        m_result.reset();
        m_row = 0;
        m_started = false;
        m_params.clear();
    }

    int column_count() const noexcept override { return m_result ? static_cast<int>(m_result->col_count) : 0; }
    ColumnType column_type(int column) const noexcept override {
        const ZephyrValue* v = cell(column);
        if (v == nullptr) {
            return ColumnType::Null;
        }
        switch (v->type) {
        case ZEPHYR_TYPE_I64:
        case ZEPHYR_TYPE_BOOL:
            return ColumnType::Integer;
        case ZEPHYR_TYPE_F64:
            return ColumnType::Float;
        case ZEPHYR_TYPE_TEXT:
            return ColumnType::Text;
        case ZEPHYR_TYPE_BYTEA:
            return ColumnType::Blob;
        default:
            return ColumnType::Null;
        }
    }
    std::int64_t column_int64(int column) const noexcept override {
        const ZephyrValue* v = cell(column);
        if (v == nullptr) return 0;
        switch (v->type) {
        case ZEPHYR_TYPE_I64: return v->as.i64;
        case ZEPHYR_TYPE_BOOL: return v->as.boolean != 0 ? 1 : 0;
        case ZEPHYR_TYPE_F64: return static_cast<std::int64_t>(v->as.f64);
        default: return 0;
        }
    }
    double column_double(int column) const noexcept override {
        const ZephyrValue* v = cell(column);
        if (v == nullptr) return 0.0;
        switch (v->type) {
        case ZEPHYR_TYPE_F64: return v->as.f64;
        case ZEPHYR_TYPE_I64: return static_cast<double>(v->as.i64);
        case ZEPHYR_TYPE_BOOL: return v->as.boolean != 0 ? 1.0 : 0.0;
        default: return 0.0;
        }
    }
    std::string_view column_text(int column) const noexcept override {
        const ZephyrValue* v = cell(column);
        if (v == nullptr || (v->type != ZEPHYR_TYPE_TEXT && v->type != ZEPHYR_TYPE_BYTEA) || v->as.slice.ptr == nullptr) {
            return {};
        }
        return std::string_view(reinterpret_cast<const char*>(v->as.slice.ptr), v->as.slice.len);
    }
    std::span<const std::byte> column_blob(int column) const noexcept override {
        const ZephyrValue* v = cell(column);
        if (v == nullptr || (v->type != ZEPHYR_TYPE_TEXT && v->type != ZEPHYR_TYPE_BYTEA) || v->as.slice.ptr == nullptr) {
            return {};
        }
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(v->as.slice.ptr), v->as.slice.len);
    }
    std::string_view column_name(int column) const noexcept override {
        if (!m_result || column < 0 || static_cast<std::uint64_t>(column) >= m_result->col_count) {
            return {};
        }
        const ZephyrColumnDesc& d = m_result->columns[column];
        return std::string_view(d.name, d.name_len);
    }
    std::string_view sql() const noexcept override { return m_sql; }
    void* native_handle() const noexcept override { return m_stmt.get(); }

private:
    struct Param {
        std::int32_t type = ZEPHYR_TYPE_NULL;
        std::int64_t i64 = 0;
        double f64 = 0.0;
        std::string bytes;
    };

    template <typename Fn>
    DbResult<void> set(int index, Fn&& fn) noexcept {
        if (index < 1 || index > 32767) {
            return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "parameter index out of range"});
        }
        try {
            if (m_params.size() < static_cast<std::size_t>(index)) {
                m_params.resize(static_cast<std::size_t>(index));
            }
            fn(m_params[static_cast<std::size_t>(index) - 1]);
        } catch (...) {
            return std::unexpected(DbError{.code = ZEPHYR_ERR_INTERNAL, .extended_code = 0, .message = "out of memory binding a parameter"});
        }
        return {};
    }

    const ZephyrValue* cell(int column) const noexcept {
        if (!m_result || column < 0 || static_cast<std::uint64_t>(column) >= m_result->col_count || m_row >= m_result->row_count) {
            return nullptr;
        }
        return &m_result->values[m_row * m_result->col_count + static_cast<std::uint64_t>(column)];
    }

    void build_values() {
        m_values.resize(m_params.size());
        for (std::size_t i = 0; i < m_params.size(); ++i) {
            ZephyrValue& v = m_values[i];
            v = ZephyrValue{};
            v.type = m_params[i].type;
            switch (m_params[i].type) {
            case ZEPHYR_TYPE_I64: v.as.i64 = m_params[i].i64; break;
            case ZEPHYR_TYPE_BOOL: v.as.boolean = static_cast<std::uint8_t>(m_params[i].i64 != 0 ? 1 : 0); break;
            case ZEPHYR_TYPE_F64: v.as.f64 = m_params[i].f64; break;
            case ZEPHYR_TYPE_TEXT:
            case ZEPHYR_TYPE_BYTEA:
                v.as.slice.ptr = reinterpret_cast<const std::uint8_t*>(m_params[i].bytes.data());
                v.as.slice.len = static_cast<std::uint32_t>(m_params[i].bytes.size());
                v.as.slice.reserved = 0;
                break;
            default: break;
            }
        }
    }

    /// m_sql with every $N outside quotes replaced by the literal of parameter N.
    DbResult<std::string> inline_sql() const noexcept {
        try {
            std::string out;
            out.reserve(m_sql.size() + 64);
            const std::string_view s = m_sql;
            for (std::size_t i = 0; i < s.size();) {
                const char c = s[i];
                if (c == '\'' || c == '"') {
                    const auto end = s.find(c, i + 1);
                    const std::size_t stop = end == std::string_view::npos ? s.size() : end + 1;
                    out.append(s.substr(i, stop - i));
                    i = stop;
                    continue;
                }
                if (c == '$' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
                    std::size_t j = i + 1;
                    std::size_t n = 0;
                    while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                        n = n * 10 + static_cast<std::size_t>(s[j] - '0');
                        ++j;
                    }
                    if (n == 0 || n > m_params.size()) {
                        return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "parameter $" + std::to_string(n) + " is not bound"});
                    }
                    if (auto lit = literal(m_params[n - 1]); lit) {
                        out += *lit;
                    } else {
                        return std::unexpected(std::move(lit.error()));
                    }
                    i = j;
                    continue;
                }
                out.push_back(c);
                ++i;
            }
            return out;
        } catch (...) {
            return std::unexpected(DbError{.code = ZEPHYR_ERR_INTERNAL, .extended_code = 0, .message = "out of memory"});
        }
    }

    static DbResult<std::string> literal(const Param& p) {
        switch (p.type) {
        case ZEPHYR_TYPE_I64:
            if (p.i64 == std::numeric_limits<std::int64_t>::min()) {
                return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "INT64_MIN cannot be written as a ZephyrDB literal"});
            }
            return std::to_string(p.i64);
        case ZEPHYR_TYPE_BOOL:
            return std::string(p.i64 != 0 ? "TRUE" : "FALSE");
        case ZEPHYR_TYPE_F64: {
            if (!std::isfinite(p.f64)) {
                return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "ZephyrDB cannot store NaN / infinity"});
            }
            std::string text = std::format("{}", p.f64); // shortest round-trip form
            if (text.find_first_of(".eE") == std::string::npos) {
                text += ".0";
            }
            return text;
        }
        case ZEPHYR_TYPE_TEXT:
        case ZEPHYR_TYPE_BYTEA: {
            if (p.bytes.find('\0') != std::string::npos) {
                return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0,
                                               .message = "ZephyrDB cannot durably write text / blob values containing NUL bytes (encode them, e.g. base64)"});
            }
            std::string out;
            out.reserve(p.bytes.size() + 2);
            out.push_back('\'');
            for (const char ch : p.bytes) {
                if (ch == '\'') {
                    out.push_back('\'');
                }
                out.push_back(ch);
            }
            out.push_back('\'');
            return out;
        }
        default:
            return std::string("NULL");
        }
    }

    ZephyrConnection* m_conn;
    ZephyrDB* m_db;
    std::string m_sql;
    std::unique_ptr<ZephyrStmt, StmtFree> m_stmt;
    bool m_inline_params = false;
    std::vector<Param> m_params;
    std::vector<ZephyrValue> m_values;
    UniqueResult m_result;
    std::uint64_t m_row = 0;
    bool m_started = false;
};

class ZephyrConnection final : public detail::ConnectionImpl {
public:
    ZephyrConnection(ZephyrDB* db, std::wstring key, HANDLE lock, bool checkpoint_on_close, bool persistent)
        : m_db(db), m_key(std::move(key)), m_lock(lock), m_checkpoint_on_close(checkpoint_on_close), m_persistent(persistent) {}

    ~ZephyrConnection() override {
        if (m_checkpoint_on_close && m_persistent) {
            zephyr_checkpoint(m_db);
        }
        zephyr_db_close(m_db);
        if (m_lock != nullptr && m_lock != INVALID_HANDLE_VALUE) {
            CloseHandle(m_lock);
        }
        if (!m_key.empty()) {
            std::scoped_lock lock(registry_mutex());
            registry().erase(m_key);
        }
    }
    ZephyrConnection(const ZephyrConnection&) = delete;
    ZephyrConnection& operator=(const ZephyrConnection&) = delete;

    Backend backend() const noexcept override { return Backend::Zephyr; }

    DbResult<void> exec(std::string_view sql) override {
        const std::string text(sql);
        std::uint64_t rows = 0;
        const int32_t status = zephyr_exec(m_db, text.c_str(), &rows);
        if (status != ZEPHYR_OK) {
            return std::unexpected(zerror(m_db, status, text));
        }
        m_changes = static_cast<std::int64_t>(rows);
        return {};
    }

    DbResult<std::unique_ptr<detail::StatementImpl>> prepare(std::string_view sql, bool /*persistent*/) override {
        std::string text(sql);
        ZephyrStmt* stmt = zephyr_prepare(m_db, text.c_str());
        // Statements ZephyrDB cannot prepare (transaction control, DDL) run through zephyr_query_params.
        return std::make_unique<ZephyrStatement>(*this, m_db, std::move(text), stmt, m_persistent);
    }

    DbResult<void> begin() override { return exec("BEGIN"); }
    DbResult<void> commit() override { return exec("COMMIT"); }
    DbResult<void> rollback() override { return exec("ROLLBACK"); }
    DbResult<void> savepoint(int) override {
        return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "ZephyrDB has no savepoints"});
    }
    DbResult<void> release_savepoint(int, bool) override { return savepoint(0); }
    std::int64_t last_insert_rowid() const noexcept override { return 0; }
    std::int64_t changes() const noexcept override { return m_changes; }
    void* native_handle() const noexcept override { return m_db; }

    DbResult<void> checkpoint() override {
        const int32_t status = zephyr_checkpoint(m_db);
        if (status != ZEPHYR_OK) {
            return std::unexpected(zerror(m_db, status, "checkpoint"));
        }
        return {};
    }

    DbResult<bool> integrity_check() override {
        const ZephyrStats stats = zephyr_get_stats(m_db);
        return stats.active_txns <= 1;
    }

    DbResult<void> backup_to(const std::filesystem::path& target) override {
        if (!m_persistent) {
            return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "in-memory ZephyrDB has no files to back up"});
        }
        if (auto r = checkpoint(); !r) {
            return r;
        }
        std::error_code ec;
        std::filesystem::create_directories(target, ec);
        for (const wchar_t* name : {L"zephyr.snap", L"zephyr.wal"}) {
            const std::filesystem::path src = m_dir / name;
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::copy_file(src, target / name, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    return std::unexpected(DbError{.code = ZEPHYR_ERR_IO, .extended_code = 0, .message = "backup copy failed: " + ec.message()});
                }
            }
        }
        return {};
    }

    void set_changes(std::int64_t n) noexcept { m_changes = n; }
    void set_dir(std::filesystem::path dir) { m_dir = std::move(dir); }

private:
    ZephyrDB* m_db;
    std::wstring m_key;
    HANDLE m_lock;
    bool m_checkpoint_on_close;
    bool m_persistent;
    std::filesystem::path m_dir;
    std::int64_t m_changes = 0;
};

DbResult<bool> ZephyrStatement::step() noexcept {
    if (!m_started) {
        m_started = true;
        m_row = 0;
        try {
            build_values();
        } catch (...) {
            return std::unexpected(DbError{.code = ZEPHYR_ERR_INTERNAL, .extended_code = 0, .message = "out of memory"});
        }
        const ZephyrValue* params = m_values.empty() ? nullptr : m_values.data();
        const auto count = static_cast<std::uint32_t>(m_values.size());
            ZephyrResultSet* raw = nullptr;
        if (m_inline_params && count > 0) {
            auto literal = inline_sql();
            if (!literal) {
                return std::unexpected(std::move(literal.error()));
            }
            raw = zephyr_query_params(m_db, literal->c_str(), nullptr, 0);
        } else {
            raw = m_stmt ? zephyr_stmt_query(m_stmt.get(), params, count) : zephyr_query_params(m_db, m_sql.c_str(), params, count);
        }
        m_result.reset(raw);
        if (!m_result) {
            return std::unexpected(zerror(m_db, ZEPHYR_ERR_INTERNAL, m_sql));
        }
        if (m_result->status != ZEPHYR_OK) {
            DbError err = zerror(m_db, static_cast<int>(m_result->status), m_sql);
            m_result.reset();
            return std::unexpected(std::move(err));
        }
        m_conn->set_changes(static_cast<std::int64_t>(m_result->rows_affected));
        return m_result->row_count > 0;
    }
    if (!m_result) {
        return false;
    }
    ++m_row;
    return m_row < m_result->row_count;
}

DbResult<std::int64_t> ZephyrStatement::execute() noexcept {
    try {
        build_values();
    } catch (...) {
        return std::unexpected(DbError{.code = ZEPHYR_ERR_INTERNAL, .extended_code = 0, .message = "out of memory"});
    }
    const ZephyrValue* params = m_values.empty() ? nullptr : m_values.data();
    const auto count = static_cast<std::uint32_t>(m_values.size());
    std::uint64_t affected = 0;
    if (m_inline_params && count > 0) {
        auto literal = inline_sql();
        if (!literal) {
            return std::unexpected(std::move(literal.error()));
        }
        const int32_t status = zephyr_exec(m_db, literal->c_str(), &affected);
        if (status != ZEPHYR_OK) {
            return std::unexpected(zerror(m_db, status, m_sql));
        }
    } else if (m_stmt) {
        const int32_t status = zephyr_stmt_exec(m_stmt.get(), params, count, &affected);
        if (status != ZEPHYR_OK) {
            return std::unexpected(zerror(m_db, status, m_sql));
        }
    } else {
        UniqueResult result(zephyr_query_params(m_db, m_sql.c_str(), params, count));
        if (!result) {
            return std::unexpected(zerror(m_db, ZEPHYR_ERR_INTERNAL, m_sql));
        }
        if (result->status != ZEPHYR_OK) {
            return std::unexpected(zerror(m_db, static_cast<int>(result->status), m_sql));
        }
        affected = result->rows_affected;
    }
    m_result.reset();
    m_started = false;
    m_conn->set_changes(static_cast<std::int64_t>(affected));
    return static_cast<std::int64_t>(affected);
}

} // namespace

namespace detail {

std::string_view zephyr_library_version() noexcept { return "ZephyrDB " BOSON_ZEPHYR_COMMIT; }

DbResult<std::unique_ptr<ConnectionImpl>> open_zephyr(const DatabaseOptions& options) {
    if (options.read_only) {
        return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0, .message = "ZephyrDB has no read-only mode"});
    }
    if (options.synchronous == Synchronous::Full || options.synchronous == Synchronous::Extra) {
        return std::unexpected(DbError{.code = ZEPHYR_ERR_INVALID_ARG, .extended_code = 0,
                                       .message = "ZephyrDB commits single statements asynchronously; use Synchronous::Normal and "
                                                  "Connection::transaction() (COMMIT waits for the WAL fsync) for durable writes"});
    }
    const bool persistent = !options.path.empty();
    std::wstring key;
    HANDLE lock = nullptr;
    std::string dir_utf8;
    if (persistent) {
        std::error_code ec;
        std::filesystem::create_directories(options.path, ec);
        if (ec) {
            return std::unexpected(DbError{.code = ZEPHYR_ERR_IO, .extended_code = 0, .message = "cannot create data directory: " + ec.message()});
        }
        key = dir_key(options.path);
        {
            std::scoped_lock reg(registry_mutex());
            if (!registry().insert(key).second) {
                return std::unexpected(DbError{.code = ZEPHYR_ERR_BUSY, .extended_code = 0, .message = "this ZephyrDB directory is already open in this process"});
            }
        }
        // Cross-process guard: ZephyrDB does not lock its files.
        const std::filesystem::path lock_path = options.path / L"boson.lock";
        lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            std::scoped_lock reg(registry_mutex());
            registry().erase(key);
            return std::unexpected(DbError{.code = ZEPHYR_ERR_BUSY, .extended_code = static_cast<int>(code),
                                           .message = "this ZephyrDB directory is in use by another process"});
        }
        dir_utf8 = core::wide_to_utf8(std::filesystem::absolute(options.path, ec).native());
    }
    ZephyrConfig cfg = zephyr_default_config();
    cfg.data_dir = persistent ? dir_utf8.c_str() : nullptr;
    cfg.enable_wal = options.wal ? 1u : 0u;
    cfg.enable_compactor = options.zephyr_compactor ? 1u : 0u;
    cfg.wal_flush_ms = static_cast<std::uint32_t>(std::clamp<long long>(options.zephyr_wal_idle_flush.count(), 1, 0x7FFFFFFF));
    ZephyrDB* db = options.encryption_key.empty()
        ? zephyr_db_open(&cfg)
        : zephyr_db_open_encrypted(&cfg, reinterpret_cast<const std::uint8_t*>(options.encryption_key.data()),
                                   static_cast<std::uint32_t>(options.encryption_key.size()));
    if (db == nullptr) {
        if (lock != nullptr) {
            CloseHandle(lock);
        }
        if (!key.empty()) {
            std::scoped_lock reg(registry_mutex());
            registry().erase(key);
        }
        return std::unexpected(DbError{.code = ZEPHYR_ERR_AUTH, .extended_code = 0,
                                       .message = options.encryption_key.empty() ? "zephyr_db_open failed (encrypted data without a key?)"
                                                                                 : "zephyr_db_open_encrypted failed (wrong key or tampered files)"});
    }
    auto conn = std::make_unique<ZephyrConnection>(db, std::move(key), lock, options.zephyr_checkpoint_on_close, persistent);
    if (persistent) {
        conn->set_dir(options.path);
    }
    return std::unique_ptr<ConnectionImpl>(std::move(conn));
}

} // namespace detail

} // namespace boson::db
