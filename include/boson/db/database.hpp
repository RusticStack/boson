#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <stdexec/execution.hpp>

// Which backends are linked: set by CMake (BOSON_DB_SQLITE / BOSON_DB_ZEPHYR).
#ifndef BOSON_DB_HAS_SQLITE
#define BOSON_DB_HAS_SQLITE 1
#endif
#ifndef BOSON_DB_HAS_ZEPHYR
#define BOSON_DB_HAS_ZEPHYR 0
#endif

namespace boson::db {

/// @brief Storage engine behind a Database. Both speak the same Boson API (Statement, Table<T>, KvStore);
/// raw SQL you write yourself must stay inside the common subset (see docs/NATIVE_SERVICES.md):
/// positional "$1".."$N" parameters, CREATE TABLE / INDEX, INSERT, UPDATE, DELETE, SELECT with WHERE /
/// ORDER BY / LIMIT, BEGIN / COMMIT / ROLLBACK.
enum class Backend : std::uint8_t {
    Sqlite, ///< SQLite amalgamation, WAL journal, one file
    Zephyr, ///< ZephyrDB (in-memory B-trees + WAL + snapshot in a directory)
};

[[nodiscard]] std::string_view to_string(Backend backend) noexcept;
[[nodiscard]] constexpr bool backend_available(Backend backend) noexcept {
    return backend == Backend::Sqlite ? BOSON_DB_HAS_SQLITE != 0 : BOSON_DB_HAS_ZEPHYR != 0;
}
/// @brief The backend Database::open uses when DatabaseOptions::backend is unset: SQLite when linked
/// (see the benchmark and durability notes in docs/NATIVE_SERVICES.md), else ZephyrDB.
[[nodiscard]] constexpr Backend default_backend() noexcept {
    return BOSON_DB_HAS_SQLITE ? Backend::Sqlite : Backend::Zephyr;
}

/// @brief Failure. code is the backend's result code (SQLITE_* or ZephyrStatus, 0 for wrapper errors).
struct DbError {
    int code = 0;
    int extended_code = 0;
    std::string message;
};

template <typename T>
using DbResult = std::expected<T, DbError>;

using Blob = std::vector<std::byte>;

enum class ColumnType : std::uint8_t {
    Integer = 1,
    Float = 2,
    Text = 3,
    Blob = 4,
    Null = 5,
};

/// Logical column kinds of Table<T>; Dialect maps them to backend type names.
enum class ColumnKind : std::uint8_t {
    Integer,
    Real,
    Boolean,
    Text,
    Blob,
};

/// @brief SQL differences Table<T> and KvStore paper over.
struct Dialect {
    Backend backend = Backend::Sqlite;
    std::string_view integer_type;
    std::string_view real_type;
    std::string_view boolean_type;
    std::string_view text_type;
    std::string_view blob_type;
    bool native_upsert = false;      ///< INSERT ... ON CONFLICT DO UPDATE
    bool alter_add_column = false;   ///< ALTER TABLE ADD COLUMN (schema evolution)
    bool savepoints = false;         ///< nested transactions
    bool limit_parameters = false;   ///< LIMIT $n (otherwise the limit is inlined)

    [[nodiscard]] std::string_view type_name(ColumnKind kind) const noexcept {
        switch (kind) {
        case ColumnKind::Integer: return integer_type;
        case ColumnKind::Real: return real_type;
        case ColumnKind::Boolean: return boolean_type;
        case ColumnKind::Text: return text_type;
        case ColumnKind::Blob: return blob_type;
        }
        return text_type;
    }
};

[[nodiscard]] const Dialect& dialect(Backend backend) noexcept;

enum class Synchronous : std::uint8_t {
    Off,
    Normal, ///< SQLite: WAL + NORMAL. Zephyr: asynchronous group commit (see docs for what a kill loses)
    Full,   ///< SQLite: fsync per commit. Zephyr: not available per statement (open fails); use transactions
    Extra,
};

struct DatabaseOptions {
    std::optional<Backend> backend; ///< unset = default_backend()
    /// SQLite: the database file. Zephyr: the data directory (zephyr.wal + zephyr.snap). Empty = in-memory.
    std::filesystem::path path;
    bool read_only = false; ///< SQLite only
    bool create = true;
    bool wal = true;        ///< SQLite journal_mode=WAL; Zephyr write-ahead log
    Synchronous synchronous = Synchronous::Normal;
    bool foreign_keys = true;                         ///< SQLite only
    std::chrono::milliseconds busy_timeout{5000};     ///< SQLite only (waits inside SQLite, no polling)
    std::size_t cache_kib = 4096;                     ///< SQLite page cache
    std::size_t mmap_bytes = 64u * 1024u * 1024u;     ///< SQLite memory-mapped reads (0 = off)
    std::size_t statement_cache = 64;                 ///< prepared statements kept per connection
    /// Zephyr: encryption at rest (ChaCha20-Poly1305, key derived with HKDF). SQLite: unsupported (open fails).
    std::vector<std::byte> encryption_key;
    /// Zephyr: WAL Guardian idle wake-up period. Appends wake it at once, so this only bounds idle wake-ups.
    std::chrono::milliseconds zephyr_wal_idle_flush{std::chrono::seconds{1}};
    /// Zephyr: run ZephyrDB's background Compactor (wakes every 5 s). Off by default: Boson checkpoints
    /// on close and on Database::checkpoint() instead, so an idle app has no periodic compaction.
    bool zephyr_compactor = false;
    /// Zephyr: write a snapshot (zephyr_checkpoint) when the Database is closed, so the next open does
    /// not replay the WAL.
    bool zephyr_checkpoint_on_close = true;
};

namespace detail {

/// Backend statement. Implemented in src/db/*_backend.cpp.
class StatementImpl {
public:
    virtual ~StatementImpl() = default;
    virtual DbResult<void> bind_int(int index, std::int64_t value) noexcept = 0;
    virtual DbResult<void> bind_bool(int index, bool value) noexcept = 0;
    virtual DbResult<void> bind_double(int index, double value) noexcept = 0;
    virtual DbResult<void> bind_text(int index, std::string_view text) noexcept = 0;
    virtual DbResult<void> bind_blob(int index, std::span<const std::byte> blob) noexcept = 0;
    virtual DbResult<void> bind_null(int index) noexcept = 0;
    [[nodiscard]] virtual int parameter_count() const noexcept = 0;
    virtual DbResult<bool> step() noexcept = 0;
    virtual DbResult<std::int64_t> execute() noexcept = 0;
    /// Finish the current run and clear the bindings (called before a cached statement is reused).
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual int column_count() const noexcept = 0;
    [[nodiscard]] virtual ColumnType column_type(int column) const noexcept = 0;
    [[nodiscard]] virtual std::int64_t column_int64(int column) const noexcept = 0;
    [[nodiscard]] virtual double column_double(int column) const noexcept = 0;
    [[nodiscard]] virtual std::string_view column_text(int column) const noexcept = 0;
    [[nodiscard]] virtual std::span<const std::byte> column_blob(int column) const noexcept = 0;
    [[nodiscard]] virtual std::string_view column_name(int column) const noexcept = 0;
    [[nodiscard]] virtual std::string_view sql() const noexcept = 0;
    [[nodiscard]] virtual void* native_handle() const noexcept = 0;
};

/// Backend connection.
class ConnectionImpl {
public:
    virtual ~ConnectionImpl() = default;
    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    virtual DbResult<void> exec(std::string_view sql) = 0;
    virtual DbResult<std::unique_ptr<StatementImpl>> prepare(std::string_view sql, bool persistent) = 0;
    virtual DbResult<void> begin() = 0;
    virtual DbResult<void> commit() = 0;
    virtual DbResult<void> rollback() = 0;
    virtual DbResult<void> savepoint(int depth) = 0;
    virtual DbResult<void> release_savepoint(int depth, bool commit) = 0;
    [[nodiscard]] virtual std::int64_t last_insert_rowid() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t changes() const noexcept = 0;
    [[nodiscard]] virtual void* native_handle() const noexcept = 0;
    virtual DbResult<void> checkpoint() = 0;
    virtual DbResult<bool> integrity_check() = 0;
    virtual DbResult<void> backup_to(const std::filesystem::path& target) = 0;
};

#if BOSON_DB_HAS_SQLITE
DbResult<std::unique_ptr<ConnectionImpl>> open_sqlite(const DatabaseOptions& options);
std::string_view sqlite_library_version() noexcept;
#endif
#if BOSON_DB_HAS_ZEPHYR
DbResult<std::unique_ptr<ConnectionImpl>> open_zephyr(const DatabaseOptions& options);
std::string_view zephyr_library_version() noexcept;
#endif

struct CacheSlot {
    std::unique_ptr<StatementImpl> impl;
    bool in_use = false;
    std::uint64_t last_used = 0;
};

struct CacheEntry {
    std::size_t hash = 0;
    std::string sql;
    std::unique_ptr<CacheSlot> slot;
};

} // namespace detail

/// @brief A prepared statement. Move-only; must not outlive its Connection. Statements from
/// Connection::cached() go back to the connection's cache (reset, bindings cleared) when destroyed.
/// Parameters are 1-based and written "$1".."$N" in SQL (both backends).
class Statement {
public:
    Statement() noexcept = default;
    Statement(Statement&& other) noexcept
        : m_owned(std::move(other.m_owned)), m_impl(std::exchange(other.m_impl, nullptr)), m_slot(std::exchange(other.m_slot, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            release();
            m_owned = std::move(other.m_owned);
            m_impl = std::exchange(other.m_impl, nullptr);
            m_slot = std::exchange(other.m_slot, nullptr);
        }
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { release(); }

    [[nodiscard]] bool valid() const noexcept { return m_impl != nullptr; }

    DbResult<void> bind(int index, std::int64_t value) noexcept { return m_impl->bind_int(index, value); }
    DbResult<void> bind(int index, bool value) noexcept { return m_impl->bind_bool(index, value); }
    DbResult<void> bind(int index, double value) noexcept { return m_impl->bind_double(index, value); }
    DbResult<void> bind(int index, std::string_view text) noexcept { return m_impl->bind_text(index, text); }
    DbResult<void> bind(int index, std::span<const std::byte> blob) noexcept { return m_impl->bind_blob(index, blob); }
    DbResult<void> bind(int index, std::nullptr_t) noexcept { return m_impl->bind_null(index); }
    [[nodiscard]] int parameter_count() const noexcept { return m_impl->parameter_count(); }

    /// @brief Binds args to $1..$N via detail::bind_value (integers, bool, enums, floating point, strings,
    /// blobs, std::optional, and any other glaze-serialisable type as JSON text).
    template <typename... Args>
    DbResult<void> bind_all(const Args&... args);

    /// @brief true when a row is available, false when the statement is done.
    DbResult<bool> step() noexcept { return m_impl->step(); }
    /// @brief Runs to completion; returns the number of changed rows.
    DbResult<std::int64_t> execute() noexcept { return m_impl->execute(); }
    /// @brief Resets and clears bindings so the statement can run again.
    void reset() noexcept { m_impl->reset(); }

    [[nodiscard]] int column_count() const noexcept { return m_impl->column_count(); }
    [[nodiscard]] ColumnType column_type(int column) const noexcept { return m_impl->column_type(column); }
    [[nodiscard]] bool column_is_null(int column) const noexcept { return column_type(column) == ColumnType::Null; }
    [[nodiscard]] std::int64_t column_int64(int column) const noexcept { return m_impl->column_int64(column); }
    [[nodiscard]] double column_double(int column) const noexcept { return m_impl->column_double(column); }
    /// @brief Valid until the next step() / reset() on this statement.
    [[nodiscard]] std::string_view column_text(int column) const noexcept { return m_impl->column_text(column); }
    [[nodiscard]] std::span<const std::byte> column_blob(int column) const noexcept { return m_impl->column_blob(column); }
    [[nodiscard]] std::string_view column_name(int column) const noexcept { return m_impl->column_name(column); }
    [[nodiscard]] std::string_view sql() const noexcept { return m_impl->sql(); }
    /// @brief sqlite3_stmt* or ZephyrStmt* (nullptr for an unprepared Zephyr fallback).
    [[nodiscard]] void* native_handle() const noexcept { return m_impl->native_handle(); }

private:
    friend class Connection;
    explicit Statement(std::unique_ptr<detail::StatementImpl> owned) noexcept : m_owned(std::move(owned)), m_impl(m_owned.get()) {}
    explicit Statement(detail::CacheSlot& slot) noexcept : m_impl(slot.impl.get()), m_slot(&slot) {}
    void release() noexcept {
        if (m_slot != nullptr) {
            m_impl->reset();
            m_slot->in_use = false;
        }
        m_owned.reset();
        m_impl = nullptr;
        m_slot = nullptr;
    }

    std::unique_ptr<detail::StatementImpl> m_owned;
    detail::StatementImpl* m_impl = nullptr;
    detail::CacheSlot* m_slot = nullptr;
};

namespace detail {

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
concept blob_like = std::same_as<T, Blob> || std::same_as<T, std::vector<std::uint8_t>> ||
                    std::same_as<T, std::span<const std::byte>>;

template <typename T>
concept text_like = std::same_as<T, std::string> || std::same_as<T, std::string_view> ||
                    std::same_as<T, const char*> || std::same_as<T, char*>;

/// Column type used for a C++ type in Table<T> columns.
template <typename T>
[[nodiscard]] constexpr ColumnKind column_kind() noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional<U>::value) {
        return column_kind<typename U::value_type>();
    } else if constexpr (std::is_same_v<U, bool>) {
        return ColumnKind::Boolean;
    } else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
        return ColumnKind::Integer;
    } else if constexpr (std::is_floating_point_v<U>) {
        return ColumnKind::Real;
    } else if constexpr (blob_like<U>) {
        return ColumnKind::Blob;
    } else {
        return ColumnKind::Text; // strings, and JSON for every other type
    }
}

template <typename T>
DbResult<void> bind_value(Statement& stmt, int index, const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional<U>::value) {
        if (!value) {
            return stmt.bind(index, nullptr);
        }
        return bind_value(stmt, index, *value);
    } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
        return stmt.bind(index, nullptr);
    } else if constexpr (std::is_same_v<U, bool>) {
        return stmt.bind(index, value);
    } else if constexpr (std::is_enum_v<U>) {
        return stmt.bind(index, static_cast<std::int64_t>(static_cast<std::underlying_type_t<U>>(value)));
    } else if constexpr (std::is_integral_v<U>) {
        return stmt.bind(index, static_cast<std::int64_t>(value));
    } else if constexpr (std::is_floating_point_v<U>) {
        return stmt.bind(index, static_cast<double>(value));
    } else if constexpr (text_like<U> || (std::is_array_v<U> && std::is_same_v<std::remove_extent_t<U>, char>)) {
        return stmt.bind(index, std::string_view(value));
    } else if constexpr (std::same_as<U, Blob> || std::same_as<U, std::span<const std::byte>>) {
        return stmt.bind(index, std::span<const std::byte>(value.data(), value.size()));
    } else if constexpr (std::same_as<U, std::vector<std::uint8_t>>) {
        return stmt.bind(index, std::as_bytes(std::span<const std::uint8_t>(value.data(), value.size())));
    } else {
        std::string json;
        if (const auto ec = glz::write_json(value, json)) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "cannot serialize a bound value to JSON"});
        }
        // SQLITE_TRANSIENT copy: json dies at the end of this scope.
        return stmt.bind(index, std::string_view(json));
    }
}

template <typename T>
DbResult<T> read_value(const Statement& stmt, int column) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional<U>::value) {
        if (stmt.column_is_null(column)) {
            return U{};
        }
        auto inner = read_value<typename U::value_type>(stmt, column);
        if (!inner) {
            return std::unexpected(std::move(inner.error()));
        }
        return U{std::move(*inner)};
    } else if constexpr (std::is_same_v<U, bool>) {
        return stmt.column_int64(column) != 0;
    } else if constexpr (std::is_enum_v<U>) {
        return static_cast<U>(static_cast<std::underlying_type_t<U>>(stmt.column_int64(column)));
    } else if constexpr (std::is_integral_v<U>) {
        return static_cast<U>(stmt.column_int64(column));
    } else if constexpr (std::is_floating_point_v<U>) {
        return static_cast<U>(stmt.column_double(column));
    } else if constexpr (std::same_as<U, std::string>) {
        return std::string(stmt.column_text(column));
    } else if constexpr (std::same_as<U, Blob>) {
        const auto b = stmt.column_blob(column);
        return Blob(b.begin(), b.end());
    } else if constexpr (std::same_as<U, std::vector<std::uint8_t>>) {
        const auto b = stmt.column_blob(column);
        std::vector<std::uint8_t> out(b.size());
        std::memcpy(out.data(), b.data(), b.size());
        return out;
    } else {
        U out{};
        if (stmt.column_is_null(column)) {
            return out;
        }
        const std::string_view text = stmt.column_text(column);
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(out, text)) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0,
                                           .message = "column " + std::string(stmt.column_name(column)) + ": " + glz::format_error(ec, text)});
        }
        return out;
    }
}

template <typename R>
struct transaction_result {
    using type = DbResult<R>;
};
template <typename R>
struct transaction_result<DbResult<R>> {
    using type = DbResult<R>;
};

} // namespace detail

template <typename... Args>
DbResult<void> Statement::bind_all(const Args&... args) {
    int index = 0;
    DbResult<void> result{};
    ((result = result ? detail::bind_value(*this, ++index, args) : result), ...);
    return result;
}

/// @brief One connection. Not thread-safe: reach it through Database::with() / transaction(), which hold
/// the Database mutex.
class Connection {
public:
    /// Construction token: only Database opens connections.
    class PassKey {
        friend class Database;
        PassKey() = default;
    };
    Connection(PassKey, std::unique_ptr<detail::ConnectionImpl> impl, std::size_t cache_capacity);
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;
    ~Connection();

    [[nodiscard]] Backend backend() const noexcept { return m_impl->backend(); }
    [[nodiscard]] const Dialect& sql_dialect() const noexcept { return dialect(backend()); }

    /// @brief Runs one or more SQL statements without results (schema, PRAGMAs).
    DbResult<void> exec(std::string_view sql) { return m_impl->exec(sql); }
    /// @brief Prepares an uncached statement.
    [[nodiscard]] DbResult<Statement> prepare(std::string_view sql);
    /// @brief Prepared statement from the per-connection LRU cache (prepared once, reused).
    [[nodiscard]] DbResult<Statement> cached(std::string_view sql);

    /// @brief Cached statement, binds args, runs to completion, returns the number of changed rows.
    template <typename... Args>
    DbResult<std::int64_t> execute(std::string_view sql, const Args&... args) {
        auto stmt = cached(sql);
        if (!stmt) {
            return std::unexpected(std::move(stmt.error()));
        }
        if (auto b = stmt->bind_all(args...); !b) {
            return std::unexpected(std::move(b.error()));
        }
        return stmt->execute();
    }

    /// @brief Calls on_row(const Statement&) for every row; on_row may return false to stop early.
    /// Returns the number of rows visited.
    template <typename OnRow, typename... Args>
    DbResult<std::size_t> query(std::string_view sql, OnRow&& on_row, const Args&... args) {
        auto stmt = cached(sql);
        if (!stmt) {
            return std::unexpected(std::move(stmt.error()));
        }
        if (auto b = stmt->bind_all(args...); !b) {
            return std::unexpected(std::move(b.error()));
        }
        std::size_t rows = 0;
        while (true) {
            auto has = stmt->step();
            if (!has) {
                return std::unexpected(std::move(has.error()));
            }
            if (!*has) {
                break;
            }
            ++rows;
            if constexpr (std::is_same_v<std::invoke_result_t<OnRow&, const Statement&>, bool>) {
                if (!on_row(static_cast<const Statement&>(*stmt))) {
                    break;
                }
            } else {
                on_row(static_cast<const Statement&>(*stmt));
            }
        }
        return rows;
    }

    /// @brief First column of the first row (std::nullopt when there is no row).
    template <typename T, typename... Args>
    DbResult<std::optional<T>> query_value(std::string_view sql, const Args&... args) {
        std::optional<T> out;
        std::optional<DbError> failure;
        auto r = query(sql, [&](const Statement& s) {
            auto v = detail::read_value<T>(s, 0);
            if (v) {
                out = std::move(*v);
            } else {
                failure = std::move(v.error());
            }
            return false;
        }, args...);
        if (!r) {
            return std::unexpected(std::move(r.error()));
        }
        if (failure) {
            return std::unexpected(std::move(*failure));
        }
        return out;
    }

    /// @brief Runs fn(Connection&) inside BEGIN ... COMMIT. Rolls back when fn returns an error DbResult or
    /// throws (the exception is rethrown after the rollback). Nested calls use SAVEPOINTs where the backend
    /// has them (SQLite); on ZephyrDB they join the outer transaction, and a failed inner call makes the
    /// outer one roll back.
    template <typename Fn>
    auto transaction(Fn&& fn) -> typename detail::transaction_result<std::invoke_result_t<Fn&, Connection&>>::type {
        using R = std::invoke_result_t<Fn&, Connection&>;
        using Out = typename detail::transaction_result<R>::type;
        if (auto b = begin_transaction(); !b) {
            return std::unexpected(std::move(b.error()));
        }
        try {
            if constexpr (std::is_void_v<R>) {
                fn(*this);
                if (auto c = end_transaction(true); !c) {
                    return std::unexpected(std::move(c.error()));
                }
                return Out{};
            } else {
                R value = fn(*this);
                bool ok = true;
                if constexpr (requires { value.has_value(); }) {
                    ok = value.has_value();
                }
                if (auto c = end_transaction(ok); !c && ok) {
                    return std::unexpected(std::move(c.error()));
                }
                if constexpr (std::is_same_v<Out, R>) {
                    return value;
                } else {
                    return Out{std::move(value)};
                }
            }
        } catch (...) {
            (void)end_transaction(false);
            throw;
        }
    }

    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept { return m_impl->last_insert_rowid(); }
    [[nodiscard]] std::int64_t changes() const noexcept { return m_impl->changes(); }
    [[nodiscard]] bool in_transaction() const noexcept { return m_depth > 0; }
    /// @brief sqlite3* or ZephyrDB*.
    [[nodiscard]] void* native_handle() const noexcept { return m_impl->native_handle(); }
    [[nodiscard]] std::size_t cached_statements() const noexcept { return m_cache.size(); }
    [[nodiscard]] detail::ConnectionImpl& backend_impl() noexcept { return *m_impl; }

private:
    DbResult<void> begin_transaction();
    DbResult<void> end_transaction(bool commit);

    std::unique_ptr<detail::ConnectionImpl> m_impl;
    std::vector<detail::CacheEntry> m_cache;
    std::size_t m_cache_capacity = 64;
    std::uint64_t m_tick = 0;
    int m_depth = 0;
    bool m_rollback_only = false;
};

/// @brief An open database (one connection guarded by a mutex). Thread-safe: every call takes the mutex,
/// so calls from several pool threads are serialised. Blocking: never call from the UI thread - use
/// run_on(scheduler, fn) or the router's pool threads.
class Database {
public:
    [[nodiscard]] static DbResult<Database> open(DatabaseOptions options = {});

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    [[nodiscard]] Backend backend() const noexcept { return m_state->backend; }
    [[nodiscard]] const Dialect& sql_dialect() const noexcept { return dialect(m_state->backend); }

    /// @brief Runs fn(Connection&) with the connection locked.
    template <typename Fn>
    decltype(auto) with(Fn&& fn) {
        std::scoped_lock lock(m_state->mutex);
        return fn(*m_state->connection);
    }

    DbResult<void> exec(std::string_view sql) {
        return with([&](Connection& c) { return c.exec(sql); });
    }

    template <typename... Args>
    DbResult<std::int64_t> execute(std::string_view sql, const Args&... args) {
        return with([&](Connection& c) { return c.execute(sql, args...); });
    }

    template <typename OnRow, typename... Args>
    DbResult<std::size_t> query(std::string_view sql, OnRow&& on_row, const Args&... args) {
        return with([&](Connection& c) { return c.query(sql, std::forward<OnRow>(on_row), args...); });
    }

    template <typename T, typename... Args>
    DbResult<std::optional<T>> query_value(std::string_view sql, const Args&... args) {
        return with([&](Connection& c) { return c.template query_value<T>(sql, args...); });
    }

    template <typename Fn>
    auto transaction(Fn&& fn) {
        return with([&](Connection& c) { return c.transaction(std::forward<Fn>(fn)); });
    }

    /// @brief Sender: runs with(fn) on sched and completes with fn's result. The Database must outlive it.
    template <stdexec::scheduler Sched, typename Fn>
    [[nodiscard]] auto run_on(Sched sched, Fn fn) {
        return stdexec::schedule(std::move(sched)) |
               stdexec::then([this, fn = std::move(fn)]() mutable { return with(fn); });
    }

    /// @brief SQLite: online copy to another file (sqlite3_backup). Zephyr: checkpoint, then copy the data
    /// directory's snapshot to target (a directory).
    DbResult<void> backup_to(const std::filesystem::path& target);
    /// @brief SQLite: PRAGMA wal_checkpoint(TRUNCATE). Zephyr: zephyr_checkpoint (snapshot + WAL truncate).
    DbResult<void> checkpoint();
    /// @brief SQLite: PRAGMA integrity_check. Zephyr: a catalog / row-count self-check.
    DbResult<bool> integrity_check();

    [[nodiscard]] const DatabaseOptions& options() const noexcept { return m_state->options; }

private:
    struct State {
        DatabaseOptions options;
        Backend backend = Backend::Sqlite;
        std::mutex mutex;
        std::unique_ptr<Connection> connection;
    };
    explicit Database(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
    std::unique_ptr<State> m_state;
};

/// @brief Library version string of a backend ("3.54.1", "zephyrdb c50eea7").
[[nodiscard]] std::string_view backend_version(Backend backend) noexcept;

} // namespace boson::db
