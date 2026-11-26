#include <boson/db/database.hpp>

#include <algorithm>
#include <functional>

namespace boson::db {

std::string_view to_string(Backend backend) noexcept {
    return backend == Backend::Sqlite ? "sqlite" : "zephyr";
}

const Dialect& dialect(Backend backend) noexcept {
    static constexpr Dialect kSqlite{
        .backend = Backend::Sqlite,
        .integer_type = "INTEGER",
        .real_type = "REAL",
        .boolean_type = "INTEGER",
        .text_type = "TEXT",
        .blob_type = "BLOB",
        .native_upsert = true,
        .alter_add_column = true,
        .savepoints = true,
        .limit_parameters = true,
    };
    static constexpr Dialect kZephyr{
        .backend = Backend::Zephyr,
        .integer_type = "BIGINT",
        .real_type = "DOUBLE PRECISION",
        .boolean_type = "BOOLEAN",
        .text_type = "TEXT",
        .blob_type = "BYTEA",
        .native_upsert = false,
        .alter_add_column = false,
        .savepoints = false,
        .limit_parameters = false,
    };
    return backend == Backend::Sqlite ? kSqlite : kZephyr;
}

// ---- Connection ---------------------------------------------------------------------------------------

Connection::Connection(PassKey, std::unique_ptr<detail::ConnectionImpl> impl, std::size_t cache_capacity)
    : m_impl(std::move(impl)), m_cache_capacity(cache_capacity) {
    m_cache.reserve(std::min<std::size_t>(cache_capacity, 256));
}

Connection::~Connection() {
    m_cache.clear(); // statements before the connection they belong to
    m_impl.reset();
}

DbResult<Statement> Connection::prepare(std::string_view sql) {
    auto impl = m_impl->prepare(sql, false);
    if (!impl) {
        return std::unexpected(std::move(impl.error()));
    }
    return Statement(std::move(*impl));
}

DbResult<Statement> Connection::cached(std::string_view sql) {
    if (m_cache_capacity == 0) {
        return prepare(sql);
    }
    const std::size_t hash = std::hash<std::string_view>{}(sql);
    for (auto& entry : m_cache) {
        if (entry.hash == hash && entry.sql == sql) {
            if (entry.slot->in_use) {
                return prepare(sql); // the same SQL is already running (nested query): private copy
            }
            entry.slot->in_use = true;
            entry.slot->last_used = ++m_tick;
            return Statement(*entry.slot);
        }
    }
    auto impl = m_impl->prepare(sql, true);
    if (!impl) {
        return std::unexpected(std::move(impl.error()));
    }
    if (m_cache.size() >= m_cache_capacity) {
        auto victim = m_cache.end();
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (!it->slot->in_use && (victim == m_cache.end() || it->slot->last_used < victim->slot->last_used)) {
                victim = it;
            }
        }
        if (victim == m_cache.end()) {
            return Statement(std::move(*impl)); // everything cached is busy: do not cache this one
        }
        m_cache.erase(victim);
    }
    auto slot = std::make_unique<detail::CacheSlot>();
    slot->impl = std::move(*impl);
    slot->in_use = true;
    slot->last_used = ++m_tick;
    detail::CacheSlot& ref = *slot;
    m_cache.push_back(detail::CacheEntry{.hash = hash, .sql = std::string(sql), .slot = std::move(slot)});
    return Statement(ref);
}

DbResult<void> Connection::begin_transaction() {
    if (m_depth == 0) {
        if (auto r = m_impl->begin(); !r) {
            return r;
        }
        m_rollback_only = false;
    } else if (sql_dialect().savepoints) {
        if (auto r = m_impl->savepoint(m_depth); !r) {
            return r;
        }
    }
    ++m_depth;
    return {};
}

DbResult<void> Connection::end_transaction(bool commit) {
    if (m_depth == 0) {
        return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "no open transaction"});
    }
    --m_depth;
    if (m_depth > 0) {
        if (sql_dialect().savepoints) {
            return m_impl->release_savepoint(m_depth, commit);
        }
        if (!commit) {
            m_rollback_only = true;
        }
        return {};
    }
    if (commit && !m_rollback_only) {
        auto r = m_impl->commit();
        if (!r) {
            (void)m_impl->rollback();
        }
        return r;
    }
    const bool forced = commit && m_rollback_only;
    m_rollback_only = false;
    auto r = m_impl->rollback();
    if (forced) {
        return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "a nested transaction failed; the transaction was rolled back"});
    }
    return r;
}

// ---- Database -----------------------------------------------------------------------------------------

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;
Database::~Database() = default;

DbResult<Database> Database::open(DatabaseOptions options) {
    const Backend backend = options.backend.value_or(default_backend());
    DbResult<std::unique_ptr<detail::ConnectionImpl>> impl =
        std::unexpected(DbError{.code = 0, .extended_code = 0, .message = std::string("backend not linked: ") + std::string(to_string(backend))});
#if BOSON_DB_HAS_SQLITE
    if (backend == Backend::Sqlite) {
        impl = detail::open_sqlite(options);
    }
#endif
#if BOSON_DB_HAS_ZEPHYR
    if (backend == Backend::Zephyr) {
        impl = detail::open_zephyr(options);
    }
#endif
    if (!impl) {
        return std::unexpected(std::move(impl.error()));
    }
    auto state = std::make_unique<State>();
    state->backend = backend;
    state->options = std::move(options);
    state->connection = std::make_unique<Connection>(Connection::PassKey{}, std::move(*impl), state->options.statement_cache);
    return Database(std::move(state));
}

DbResult<void> Database::backup_to(const std::filesystem::path& target) {
    return with([&](Connection& c) { return c.backend_impl().backup_to(target); });
}

DbResult<void> Database::checkpoint() {
    return with([](Connection& c) { return c.backend_impl().checkpoint(); });
}

DbResult<bool> Database::integrity_check() {
    return with([](Connection& c) { return c.backend_impl().integrity_check(); });
}

std::string_view backend_version(Backend backend) noexcept {
#if BOSON_DB_HAS_SQLITE
    if (backend == Backend::Sqlite) {
        return detail::sqlite_library_version();
    }
#endif
#if BOSON_DB_HAS_ZEPHYR
    if (backend == Backend::Zephyr) {
        return detail::zephyr_library_version();
    }
#endif
    return "not linked";
}

} // namespace boson::db
