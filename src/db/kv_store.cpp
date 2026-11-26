#include <boson/db/kv_store.hpp>

#include <chrono>

namespace boson::db {

namespace {

constexpr char kSep = '\x1f';

std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

DbResult<std::string> make_key(std::string_view ns, std::string_view key) {
    if (ns.find(kSep) != std::string_view::npos || key.find(kSep) != std::string_view::npos) {
        return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "namespace / key must not contain U+001F"});
    }
    std::string out;
    out.reserve(ns.size() + 1 + key.size());
    out.append(ns).push_back(kSep);
    out.append(key);
    return out;
}

/// Smallest string greater than every string starting with prefix ("" -> unbounded).
std::optional<std::string> prefix_end(std::string_view prefix) {
    std::string end(prefix);
    while (!end.empty()) {
        auto& last = end.back();
        if (static_cast<unsigned char>(last) != 0xFF) {
            last = static_cast<char>(static_cast<unsigned char>(last) + 1);
            return end;
        }
        end.pop_back();
    }
    return std::nullopt;
}

} // namespace

DbResult<void> KvStore::create() {
    const Dialect& d = m_db->sql_dialect();
    return m_db->exec("CREATE TABLE IF NOT EXISTS boson_kv (k " + std::string(d.text_type) + " PRIMARY KEY, ns " +
                      std::string(d.text_type) + ", item " + std::string(d.text_type) + ", val " + std::string(d.text_type) +
                      ", updated_ms " + std::string(d.integer_type) + ")");
}

DbResult<std::optional<std::string>> KvStore::get(std::string_view ns, std::string_view key) {
    auto k = make_key(ns, key);
    if (!k) {
        return std::unexpected(std::move(k.error()));
    }
    return m_db->query_value<std::string>("SELECT val FROM boson_kv WHERE k = $1", *k);
}

DbResult<void> KvStore::set(std::string_view ns, std::string_view key, std::string_view value) {
    auto k = make_key(ns, key);
    if (!k) {
        return std::unexpected(std::move(k.error()));
    }
    const std::int64_t now = now_ms();
    return m_db->with([&](Connection& c) -> DbResult<void> {
        if (c.sql_dialect().native_upsert) {
            auto r = c.execute(
                "INSERT INTO boson_kv (k, ns, item, val, updated_ms) VALUES ($1, $2, $3, $4, $5) "
                "ON CONFLICT(k) DO UPDATE SET val = excluded.val, updated_ms = excluded.updated_ms",
                *k, ns, key, value, now);
            if (!r) {
                return std::unexpected(std::move(r.error()));
            }
            return {};
        }
        auto updated = c.execute("UPDATE boson_kv SET val = $2, updated_ms = $3 WHERE k = $1", *k, value, now);
        if (!updated) {
            return std::unexpected(std::move(updated.error()));
        }
        if (*updated == 0) {
            auto inserted = c.execute("INSERT INTO boson_kv (k, ns, item, val, updated_ms) VALUES ($1, $2, $3, $4, $5)", *k, ns, key, value, now);
            if (!inserted) {
                return std::unexpected(std::move(inserted.error()));
            }
        }
        return {};
    });
}

DbResult<bool> KvStore::remove(std::string_view ns, std::string_view key) {
    auto k = make_key(ns, key);
    if (!k) {
        return std::unexpected(std::move(k.error()));
    }
    auto r = m_db->execute("DELETE FROM boson_kv WHERE k = $1", *k);
    if (!r) {
        return std::unexpected(std::move(r.error()));
    }
    return *r > 0;
}

DbResult<std::vector<KvEntry>> KvStore::entries(std::string_view ns, std::string_view prefix, std::size_t limit) {
    auto lo = make_key(ns, prefix);
    if (!lo) {
        return std::unexpected(std::move(lo.error()));
    }
    // The upper bound always exists: the namespace separator is followed by prefix, and the bound bumps
    // the last byte (at worst the separator itself).
    const std::string hi = prefix_end(*lo).value_or(std::string{});
    std::vector<KvEntry> out;
    const std::string sql = "SELECT item, val, updated_ms FROM boson_kv WHERE k >= $1 AND k < $2 ORDER BY k LIMIT " + std::to_string(limit);
    auto r = m_db->query(sql, [&](const Statement& s) {
        out.push_back(KvEntry{.key = std::string(s.column_text(0)), .value = std::string(s.column_text(1)), .updated_ms = s.column_int64(2)});
    }, *lo, hi);
    if (!r) {
        return std::unexpected(std::move(r.error()));
    }
    return out;
}

DbResult<std::vector<std::string>> KvStore::keys(std::string_view ns, std::string_view prefix, std::size_t limit) {
    auto lo = make_key(ns, prefix);
    if (!lo) {
        return std::unexpected(std::move(lo.error()));
    }
    const std::string hi = prefix_end(*lo).value_or(std::string{});
    std::vector<std::string> out;
    const std::string sql = "SELECT item FROM boson_kv WHERE k >= $1 AND k < $2 ORDER BY k LIMIT " + std::to_string(limit);
    auto r = m_db->query(sql, [&](const Statement& s) { out.emplace_back(s.column_text(0)); }, *lo, hi);
    if (!r) {
        return std::unexpected(std::move(r.error()));
    }
    return out;
}

DbResult<std::int64_t> KvStore::clear(std::string_view ns) {
    return m_db->execute("DELETE FROM boson_kv WHERE ns = $1", ns);
}

DbResult<std::int64_t> KvStore::count(std::string_view ns) {
    auto r = m_db->query_value<std::int64_t>("SELECT COUNT(*) FROM boson_kv WHERE ns = $1", ns);
    if (!r) {
        return std::unexpected(std::move(r.error()));
    }
    return r->value_or(0);
}

} // namespace boson::db
