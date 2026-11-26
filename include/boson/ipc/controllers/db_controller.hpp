#pragma once

#include <boson/db/database.hpp>
#include <boson/db/kv_store.hpp>
#include <boson/ipc/export.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct DbControllerOptions {
    /// Namespaces the page may use. Default deny: an empty list refuses every call.
    std::vector<std::string> namespaces;
    /// Namespaces the page may read but not change.
    std::vector<std::string> read_only_namespaces;
    std::size_t max_value_bytes = 1024 * 1024;
    std::size_t max_key_bytes = 512;
    std::size_t max_list = 10'000;
};

struct DbKeyRequest {
    std::string ns;
    std::string key;
};

struct DbSetRequest {
    std::string ns;
    std::string key;
    /// JSON text (validated); the JS helper serialises values for you.
    std::string value;
};

struct DbListRequest {
    std::string ns;
    std::string prefix;
    std::optional<std::uint32_t> limit;
};

struct DbNsRequest {
    std::string ns;
};

struct DbGetResponse {
    bool found = false;
    std::string value; ///< JSON text ("null" when not found)
};

struct DbBoolResponse {
    bool value = false;
};

struct DbCountResponse {
    std::int64_t count = 0;
};

struct DbKeysResponse {
    std::vector<std::string> keys;
};

struct DbEntriesResponse {
    std::vector<db::KvEntry> entries;
};

struct DbInfoResponse {
    std::string backend;
    std::string version;
    std::vector<std::string> namespaces;
    std::vector<std::string> read_only_namespaces;
};

/// @brief JS "Db": namespaced key/value storage over boson::db (SQLite or ZephyrDB). Values are JSON. Only
/// namespaces granted in DbControllerOptions are reachable (default deny); SQL is never exposed to the
/// page. Runs on the router's pool thread.
class DbController {
public:
    BOSON_CONTROLLER("Db");

    DbController(std::shared_ptr<db::Database> database, DbControllerOptions options = {})
        : m_db(std::move(database)), m_options(std::move(options)) {
        if (!m_db) {
            throw std::invalid_argument("DbController needs a database");
        }
        m_kv = std::make_unique<db::KvStore>(*m_db);
    }

    [[nodiscard]] DbInfoResponse info() const {
        return DbInfoResponse{
            .backend = std::string(db::to_string(m_db->backend())),
            .version = std::string(db::backend_version(m_db->backend())),
            .namespaces = m_options.namespaces,
            .read_only_namespaces = m_options.read_only_namespaces,
        };
    }

    [[nodiscard]] DbGetResponse get(DbKeyRequest req) {
        check(req.ns, false);
        check_key(req.key);
        auto value = unwrap(kv().get(req.ns, req.key));
        if (!value) {
            return DbGetResponse{.found = false, .value = "null"};
        }
        return DbGetResponse{.found = true, .value = std::move(*value)};
    }

    void set(DbSetRequest req) {
        check(req.ns, true);
        check_key(req.key);
        if (req.value.size() > m_options.max_value_bytes) {
            throw std::runtime_error("value larger than " + std::to_string(m_options.max_value_bytes) + " bytes");
        }
        if (glz::validate_json(req.value)) {
            throw std::runtime_error("value is not valid JSON");
        }
        unwrap(kv().set(req.ns, req.key, req.value));
    }

    [[nodiscard]] DbBoolResponse remove(DbKeyRequest req) {
        check(req.ns, true);
        check_key(req.key);
        return DbBoolResponse{.value = unwrap(kv().remove(req.ns, req.key))};
    }

    [[nodiscard]] DbKeysResponse keys(DbListRequest req) {
        check(req.ns, false);
        return DbKeysResponse{.keys = unwrap(kv().keys(req.ns, req.prefix, limit(req.limit)))};
    }

    [[nodiscard]] DbEntriesResponse entries(DbListRequest req) {
        check(req.ns, false);
        return DbEntriesResponse{.entries = unwrap(kv().entries(req.ns, req.prefix, limit(req.limit)))};
    }

    [[nodiscard]] DbCountResponse clear(DbNsRequest req) {
        check(req.ns, true);
        return DbCountResponse{.count = unwrap(kv().clear(req.ns))};
    }

    [[nodiscard]] DbCountResponse count(DbNsRequest req) {
        check(req.ns, false);
        return DbCountResponse{.count = unwrap(kv().count(req.ns))};
    }

private:
    db::KvStore& kv() {
        std::call_once(m_created, [this]() { unwrap(m_kv->create()); });
        return *m_kv;
    }

    template <typename T>
    static T unwrap(db::DbResult<T> r) {
        if (!r) {
            throw std::runtime_error(r.error().message);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(*r);
        }
    }

    void check(const std::string& ns, bool write) const {
        const auto has = [&](const std::vector<std::string>& list) { return std::ranges::find(list, ns) != list.end(); };
        const bool read_only = has(m_options.read_only_namespaces);
        if (!has(m_options.namespaces) && !read_only) {
            throw std::runtime_error("permission denied: namespace '" + ns + "' is not granted");
        }
        if (write && read_only) {
            throw std::runtime_error("permission denied: namespace '" + ns + "' is read-only");
        }
    }

    void check_key(const std::string& key) const {
        if (key.empty() || key.size() > m_options.max_key_bytes) {
            throw std::runtime_error("key must be 1.." + std::to_string(m_options.max_key_bytes) + " bytes");
        }
    }

    [[nodiscard]] std::size_t limit(std::optional<std::uint32_t> requested) const {
        return std::min<std::size_t>(requested.value_or(static_cast<std::uint32_t>(m_options.max_list)), m_options.max_list);
    }

    std::shared_ptr<db::Database> m_db;
    DbControllerOptions m_options;
    std::unique_ptr<db::KvStore> m_kv;
    std::once_flag m_created;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::DbController> {
    using T = boson::ipc::DbController;
    static constexpr auto value = glz::object(
        "info", &T::info,
        "get", &T::get,
        "set", &T::set,
        "remove", &T::remove,
        "keys", &T::keys,
        "entries", &T::entries,
        "clear", &T::clear,
        "count", &T::count);
};
