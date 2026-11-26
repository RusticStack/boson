#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace boson::net {

struct HttpResponse;

struct HttpCacheOptions {
    std::size_t max_entries = 256;
    std::size_t max_bytes = 16u * 1024u * 1024u; ///< body bytes over all entries
    std::size_t max_entry_bytes = 2u * 1024u * 1024u;
    std::chrono::seconds default_ttl{60};
    /// Use Cache-Control max-age / no-store / no-cache / private from the response when present.
    bool respect_cache_control = true;
};

struct HttpCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stores = 0;
    std::uint64_t evictions = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

/// @brief In-memory LRU response cache with a per-entry TTL. Thread-safe. Entries are shared immutable
/// responses (a hit copies no body: callers get a shared_ptr). Expired entries are dropped lazily on
/// lookup - there is no background timer.
class HttpCache {
public:
    using Clock = std::chrono::steady_clock;

    explicit HttpCache(HttpCacheOptions options = {});
    ~HttpCache();
    HttpCache(const HttpCache&) = delete;
    HttpCache& operator=(const HttpCache&) = delete;

    [[nodiscard]] std::shared_ptr<const HttpResponse> lookup(std::string_view key, Clock::time_point now = Clock::now());
    /// @brief Stores response under key unless its size / Cache-Control forbid it. Returns true if stored.
    bool store(std::string key, std::shared_ptr<const HttpResponse> response, Clock::time_point now = Clock::now());
    /// @brief TTL the cache would give this response (std::nullopt = not cacheable).
    [[nodiscard]] std::optional<std::chrono::seconds> ttl_for(const HttpResponse& response) const;
    void erase(std::string_view key);
    void clear();
    [[nodiscard]] HttpCacheStats stats() const;
    [[nodiscard]] const HttpCacheOptions& options() const noexcept { return m_options; }

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const HttpResponse> response;
        Clock::time_point expires;
        std::size_t bytes = 0;
    };
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    void evict_locked();

    HttpCacheOptions m_options;
    mutable std::mutex m_mutex;
    std::list<Entry> m_lru; ///< front = most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator, Hash, std::equal_to<>> m_index;
    std::size_t m_bytes = 0;
    HttpCacheStats m_stats;
};

} // namespace boson::net
