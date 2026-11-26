#pragma once

#include <boson/db/database.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::db {

struct KvEntry {
    std::string key;
    std::string value;
    std::int64_t updated_ms = 0; ///< Unix epoch milliseconds
};

/// @brief Namespaced key/value store in one table ("boson_kv": primary key k = ns + U+001F + key, plus
/// ns / item / val / updated_ms columns), identical on both backends. Values are UTF-8 text (typically
/// JSON); get_as / set_as convert glaze types. Prefix listing is a primary-key range scan. Thread-safe
/// through Database.
class KvStore {
public:
    explicit KvStore(Database& db) : m_db(&db) {}

    /// @brief Creates the table (idempotent). Call once at startup.
    DbResult<void> create();

    [[nodiscard]] DbResult<std::optional<std::string>> get(std::string_view ns, std::string_view key);
    DbResult<void> set(std::string_view ns, std::string_view key, std::string_view value);
    DbResult<bool> remove(std::string_view ns, std::string_view key);
    /// @brief Keys of ns starting with prefix, ordered, at most limit.
    [[nodiscard]] DbResult<std::vector<std::string>> keys(std::string_view ns, std::string_view prefix = {}, std::size_t limit = 10'000);
    [[nodiscard]] DbResult<std::vector<KvEntry>> entries(std::string_view ns, std::string_view prefix = {}, std::size_t limit = 10'000);
    DbResult<std::int64_t> clear(std::string_view ns);
    [[nodiscard]] DbResult<std::int64_t> count(std::string_view ns);

    template <typename T>
    [[nodiscard]] DbResult<std::optional<T>> get_as(std::string_view ns, std::string_view key) {
        auto raw = get(ns, key);
        if (!raw) {
            return std::unexpected(std::move(raw.error()));
        }
        if (!*raw) {
            return std::optional<T>{};
        }
        T value{};
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(value, **raw)) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = glz::format_error(ec, **raw)});
        }
        return std::optional<T>{std::move(value)};
    }

    template <typename T>
    DbResult<void> set_as(std::string_view ns, std::string_view key, const T& value) {
        std::string json;
        if (const auto ec = glz::write_json(value, json)) {
            return std::unexpected(DbError{.code = 0, .extended_code = 0, .message = "cannot serialize value"});
        }
        return set(ns, key, json);
    }

private:
    Database* m_db;
};

} // namespace boson::db
