#pragma once

#include <boson/db/database.hpp>

#include <glaze/glaze.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::db {

/// @brief Typed record table mapped from a glaze-reflected aggregate. One column per field, named after
/// the field: integers / bool / enums -> INTEGER, floating point -> REAL, std::string -> TEXT,
/// std::vector<std::byte|uint8_t> -> BLOB, std::optional<X> -> nullable X, anything else -> JSON TEXT.
/// The primary key defaults to the first field. Every statement is built once in the constructor and runs
/// from the connection's prepared-statement cache.
///
/// Schema evolution: create() adds columns for fields that are missing from an existing table (ALTER TABLE
/// ADD COLUMN); columns that no longer have a field are left alone.
template <typename T>
    requires glz::reflectable<T>
class Table {
public:
    static constexpr auto field_names = glz::reflect<T>::keys;
    static constexpr std::size_t field_count = field_names.size();

    Table(Database& db, std::string name, std::string_view primary_key = field_names[0])
        : m_db(&db), m_name(std::move(name)), m_dialect(&db.sql_dialect()) {
        m_valid_name = valid_identifier(m_name);
        m_pk = field_count;
        for (std::size_t i = 0; i < field_count; ++i) {
            if (field_names[i] == primary_key) {
                m_pk = i;
            }
        }
        build_sql();
    }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] std::string_view primary_key() const noexcept { return m_pk < field_count ? field_names[m_pk] : std::string_view{}; }

    /// @brief CREATE TABLE IF NOT EXISTS + missing columns.
    DbResult<void> create() {
        if (auto v = validate(); !v) {
            return v;
        }
        return m_db->with([&](Connection& c) -> DbResult<void> {
            if (auto r = c.exec(m_create_sql); !r) {
                return r;
            }
            if (!c.sql_dialect().alter_add_column) {
                return {};
            }
            std::vector<std::string> existing;
            auto listed = c.query("SELECT name FROM pragma_table_info($1)", [&](const Statement& s) {
                existing.emplace_back(s.column_text(0));
            }, m_name);
            if (!listed) {
                return std::unexpected(std::move(listed.error()));
            }
            for (std::size_t i = 0; i < field_count; ++i) {
                bool present = false;
                for (const auto& col : existing) {
                    present = present || col == field_names[i];
                }
                if (!present) {
                    if (auto r = c.exec("ALTER TABLE " + quote(m_name) + " ADD COLUMN " + quote(field_names[i]) + " " +
                                        std::string(m_dialect->type_name(m_kinds[i]))); !r) {
                        return r;
                    }
                }
            }
            return {};
        });
    }

    /// @brief CREATE [UNIQUE] INDEX IF NOT EXISTS <table>_<column> ON <table>(<column>).
    DbResult<void> create_index(std::string_view column, bool unique = false) {
        if (!valid_identifier(column)) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "invalid column name"});
        }
        return m_db->exec(std::string("CREATE ") + (unique ? "UNIQUE " : "") + "INDEX IF NOT EXISTS " +
                          quote(m_name + "_" + std::string(column)) + " ON " + quote(m_name) + "(" + quote(column) + ")");
    }

    /// @brief INSERT ... ON CONFLICT(pk) DO UPDATE (insert or replace one record).
    DbResult<void> upsert(const T& record) {
        return m_db->with([&](Connection& c) { return upsert(c, record); });
    }
    DbResult<void> upsert(Connection& c, const T& record) {
        if (auto v = validate(); !v) {
            return v;
        }
        if (m_dialect->native_upsert) {
            auto r = run_record(c, m_upsert_sql, record);
            if (!r) {
                return std::unexpected(std::move(r.error()));
            }
            return {};
        }
        // No ON CONFLICT (ZephyrDB): UPDATE by primary key, INSERT when nothing matched. Atomic for Boson
        // callers because the connection is held for both statements.
        if (!m_update_sql.empty()) {
            auto updated = run_record(c, m_update_sql, record);
            if (!updated) {
                return std::unexpected(std::move(updated.error()));
            }
            if (*updated > 0) {
                return {};
            }
        } else {
            auto probe = c.cached(m_get_sql);
            if (!probe) {
                return std::unexpected(std::move(probe.error()));
            }
            if (auto b = bind_pk(*probe, record); !b) {
                return b;
            }
            auto found = probe->step();
            if (!found) {
                return std::unexpected(std::move(found.error()));
            }
            if (*found) {
                return {}; // the record is only its key and already exists
            }
        }
        auto inserted = run_record(c, m_insert_sql, record);
        if (!inserted) {
            return std::unexpected(std::move(inserted.error()));
        }
        return {};
    }

    /// @brief Upserts every record in one transaction (all or nothing).
    DbResult<void> upsert_all(std::span<const T> records) {
        return m_db->transaction([&](Connection& c) -> DbResult<void> {
            for (const auto& record : records) {
                if (auto r = upsert(c, record); !r) {
                    return r;
                }
            }
            return {};
        });
    }

    template <typename K>
    DbResult<std::optional<T>> get(const K& key) {
        return m_db->with([&](Connection& c) { return get(c, key); });
    }
    template <typename K>
    DbResult<std::optional<T>> get(Connection& c, const K& key) {
        if (auto v = validate(); !v) {
            return std::unexpected(std::move(v.error()));
        }
        std::optional<T> out;
        std::optional<DbError> failure;
        auto r = c.query(m_get_sql, [&](const Statement& s) {
            auto rec = read_record(s);
            if (rec) {
                out = std::move(*rec);
            } else {
                failure = std::move(rec.error());
            }
            return false;
        }, key);
        if (!r) {
            return std::unexpected(std::move(r.error()));
        }
        if (failure) {
            return std::unexpected(std::move(*failure));
        }
        return out;
    }

    template <typename K>
    DbResult<bool> remove(const K& key) {
        if (auto v = validate(); !v) {
            return std::unexpected(std::move(v.error()));
        }
        auto n = m_db->execute(m_remove_sql, key);
        if (!n) {
            return std::unexpected(std::move(n.error()));
        }
        return *n > 0;
    }

    DbResult<std::vector<T>> all() { return where({}); }

    /// @brief SELECT ... WHERE <clause> (clause may also carry ORDER BY / LIMIT). Bind values with $1..$N.
    /// The clause is SQL: never build it from untrusted text, pass values through args.
    template <typename... Args>
    DbResult<std::vector<T>> where(std::string_view clause, const Args&... args) {
        if (auto v = validate(); !v) {
            return std::unexpected(std::move(v.error()));
        }
        std::string sql = m_select_sql;
        if (!clause.empty()) {
            sql += " WHERE ";
            sql += clause;
        }
        std::vector<T> out;
        std::optional<DbError> failure;
        auto r = m_db->query(sql, [&](const Statement& s) {
            auto rec = read_record(s);
            if (!rec) {
                failure = std::move(rec.error());
                return false;
            }
            out.push_back(std::move(*rec));
            return true;
        }, args...);
        if (!r) {
            return std::unexpected(std::move(r.error()));
        }
        if (failure) {
            return std::unexpected(std::move(*failure));
        }
        return out;
    }

    DbResult<std::int64_t> count() {
        if (auto v = validate(); !v) {
            return std::unexpected(std::move(v.error()));
        }
        auto n = m_db->query_value<std::int64_t>("SELECT COUNT(*) FROM " + quote(m_name));
        if (!n) {
            return std::unexpected(std::move(n.error()));
        }
        return n->value_or(0);
    }

    DbResult<std::int64_t> clear() {
        if (auto v = validate(); !v) {
            return std::unexpected(std::move(v.error()));
        }
        return m_db->execute("DELETE FROM " + quote(m_name));
    }

    /// @brief Row -> record (columns in field order, as selected by where()/get()).
    static DbResult<T> read_record(const Statement& s) {
        T record{};
        std::optional<DbError> failure;
        auto tie = glz::to_tie(record);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((failure ? void() : [&] {
                 auto& field = glz::get<I>(tie);
                 auto v = detail::read_value<std::remove_cvref_t<decltype(field)>>(s, static_cast<int>(I));
                 if (v) {
                     field = std::move(*v);
                 } else {
                     failure = std::move(v.error());
                 }
             }()),
             ...);
        }(std::make_index_sequence<field_count>{});
        if (failure) {
            return std::unexpected(std::move(*failure));
        }
        return record;
    }

private:
    static bool valid_identifier(std::string_view s) noexcept {
        if (s.empty() || s.size() > 128) {
            return false;
        }
        for (const char c : s) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return false;
            }
        }
        return !(s.front() >= '0' && s.front() <= '9') && !s.starts_with("sqlite_");
    }

    static std::string quote(std::string_view id) {
        std::string out = "\"";
        for (const char c : id) {
            out += c;
            if (c == '"') {
                out += '"';
            }
        }
        out += '"';
        return out;
    }

    DbResult<void> validate() const {
        if (!m_valid_name) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "invalid table name: " + m_name});
        }
        if (m_pk >= field_count) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "primary key is not a field of the record"});
        }
        return {};
    }

    template <std::size_t I>
    static constexpr ColumnKind field_kind() noexcept {
        using F = std::remove_cvref_t<decltype(glz::get<I>(std::declval<typename glz::reflect<T>::tie_type>()))>;
        return detail::column_kind<F>();
    }

    DbResult<void> bind_pk(Statement& stmt, const T& record) const {
        DbResult<void> result{};
        auto tie = glz::to_tie(record);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((I == m_pk ? (void)(result = detail::bind_value(stmt, 1, glz::get<I>(tie))) : void()), ...);
        }(std::make_index_sequence<field_count>{});
        return result;
    }

    DbResult<std::int64_t> run_record(Connection& c, const std::string& sql, const T& record) {
        auto stmt = c.cached(sql);
        if (!stmt) {
            return std::unexpected(std::move(stmt.error()));
        }
        if (auto b = bind_record(*stmt, record); !b) {
            return std::unexpected(std::move(b.error()));
        }
        return stmt->execute();
    }

    void build_sql() {
        [&]<std::size_t... I>(std::index_sequence<I...>) { ((m_kinds[I] = field_kind<I>()), ...); }(std::make_index_sequence<field_count>{});
        std::string cols;
        std::string params;
        std::string updates;
        std::string defs;
        if (m_pk >= field_count) {
            return;
        }
        std::string sets;
        for (std::size_t i = 0; i < field_count; ++i) {
            const std::string col = quote(field_names[i]);
            const std::string param = "$" + std::to_string(i + 1);
            cols += (i ? ", " : "") + col;
            params += (i ? ", " : "") + param;
            defs += (i ? ", " : "") + col + " " + std::string(m_dialect->type_name(m_kinds[i])) + (i == m_pk ? " PRIMARY KEY" : "");
            if (i != m_pk) {
                updates += (updates.empty() ? "" : ", ") + col + " = excluded." + col;
                sets += (sets.empty() ? "" : ", ") + col + " = " + param;
            }
        }
        const std::string pk = quote(field_names[m_pk]);
        const std::string pk_param = "$" + std::to_string(m_pk + 1);
        m_create_sql = "CREATE TABLE IF NOT EXISTS " + quote(m_name) + " (" + defs + ")";
        m_insert_sql = "INSERT INTO " + quote(m_name) + " (" + cols + ") VALUES (" + params + ")";
        m_upsert_sql = m_insert_sql + " ON CONFLICT(" + pk + ") DO " + (updates.empty() ? std::string("NOTHING") : "UPDATE SET " + updates);
        if (!sets.empty()) {
            m_update_sql = "UPDATE " + quote(m_name) + " SET " + sets + " WHERE " + pk + " = " + pk_param;
        }
        m_select_sql = "SELECT " + cols + " FROM " + quote(m_name);
        m_get_sql = m_select_sql + " WHERE " + pk + " = $1";
        m_remove_sql = "DELETE FROM " + quote(m_name) + " WHERE " + pk + " = $1";
    }

    static DbResult<void> bind_record(Statement& stmt, const T& record) {
        DbResult<void> result{};
        auto tie = glz::to_tie(record);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((result = result ? detail::bind_value(stmt, static_cast<int>(I + 1), glz::get<I>(tie)) : result), ...);
        }(std::make_index_sequence<field_count>{});
        return result;
    }

    Database* m_db;
    std::string m_name;
    bool m_valid_name = false;
    std::size_t m_pk = 0;
    const Dialect* m_dialect;
    std::array<ColumnKind, field_count> m_kinds{};
    std::string m_create_sql;
    std::string m_insert_sql;
    std::string m_update_sql;
    std::string m_upsert_sql;
    std::string m_select_sql;
    std::string m_get_sql;
    std::string m_remove_sql;
};

} // namespace boson::db
