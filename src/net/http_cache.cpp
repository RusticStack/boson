#include <boson/net/http_cache.hpp>
#include <boson/net/http_client.hpp>

#include <algorithm>
#include <cctype>

namespace boson::net {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

HttpCache::HttpCache(HttpCacheOptions options) : m_options(options) {}
HttpCache::~HttpCache() = default;

std::optional<std::chrono::seconds> HttpCache::ttl_for(const HttpResponse& response) const {
    if (response.status != 200 || response.body.size() > m_options.max_entry_bytes) {
        return std::nullopt;
    }
    std::chrono::seconds ttl = m_options.default_ttl;
    if (m_options.respect_cache_control) {
        if (const auto cc = response.header("Cache-Control")) {
            std::string_view rest = *cc;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const std::string_view directive = trim(rest.substr(0, comma));
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                if (iequals(directive, "no-store") || iequals(directive, "no-cache")) {
                    return std::nullopt;
                }
                if (directive.size() > 8 && iequals(directive.substr(0, 8), "max-age=")) {
                    long long seconds = 0;
                    for (const char c : directive.substr(8)) {
                        if (c < '0' || c > '9') {
                            break;
                        }
                        seconds = std::min<long long>(seconds * 10 + (c - '0'), 365LL * 24 * 3600);
                    }
                    ttl = std::chrono::seconds{seconds};
                }
            }
        }
    }
    if (ttl.count() <= 0) {
        return std::nullopt;
    }
    return ttl;
}

std::shared_ptr<const HttpResponse> HttpCache::lookup(std::string_view key, Clock::time_point now) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        ++m_stats.misses;
        return nullptr;
    }
    if (it->second->expires <= now) {
        m_bytes -= it->second->bytes;
        m_lru.erase(it->second);
        m_index.erase(it);
        ++m_stats.misses;
        return nullptr;
    }
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    ++m_stats.hits;
    return it->second->response;
}

bool HttpCache::store(std::string key, std::shared_ptr<const HttpResponse> response, Clock::time_point now) {
    if (!response || m_options.max_entries == 0) {
        return false;
    }
    const auto ttl = ttl_for(*response);
    if (!ttl) {
        return false;
    }
    const std::size_t bytes = response->body.size();
    std::scoped_lock lock(m_mutex);
    if (const auto it = m_index.find(key); it != m_index.end()) {
        m_bytes -= it->second->bytes;
        m_lru.erase(it->second);
        m_index.erase(it);
    }
    m_lru.push_front(Entry{.key = key, .response = std::move(response), .expires = now + *ttl, .bytes = bytes});
    m_index.emplace(std::move(key), m_lru.begin());
    m_bytes += bytes;
    ++m_stats.stores;
    evict_locked();
    return true;
}

void HttpCache::evict_locked() {
    while (!m_lru.empty() && (m_lru.size() > m_options.max_entries || m_bytes > m_options.max_bytes)) {
        const Entry& last = m_lru.back();
        m_bytes -= last.bytes;
        m_index.erase(m_index.find(std::string_view(last.key)));
        m_lru.pop_back();
        ++m_stats.evictions;
    }
}

void HttpCache::erase(std::string_view key) {
    std::scoped_lock lock(m_mutex);
    if (const auto it = m_index.find(key); it != m_index.end()) {
        m_bytes -= it->second->bytes;
        m_lru.erase(it->second);
        m_index.erase(it);
    }
}

void HttpCache::clear() {
    std::scoped_lock lock(m_mutex);
    m_lru.clear();
    m_index.clear();
    m_bytes = 0;
}

HttpCacheStats HttpCache::stats() const {
    std::scoped_lock lock(m_mutex);
    HttpCacheStats s = m_stats;
    s.entries = m_lru.size();
    s.bytes = m_bytes;
    return s;
}

} // namespace boson::net
