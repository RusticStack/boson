#pragma once

// The native side of addon_manager_mini: fetch a JSON feed, download a zip with progress, verify its
// SHA-256, extract it through a staged install (atomic swap, rollback on failure) and keep records in SQLite.
// Everything blocking runs on the caller's thread: the controller calls it from pool threads.

#include <boson/archive/zip.hpp>
#include <boson/crypto/hash.hpp>
#include <boson/db/database.hpp>
#include <boson/db/table.hpp>
#include <boson/fs/install_transaction.hpp>
#include <boson/fs/known_folders.hpp>
#include <boson/net/http_client.hpp>

#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace addon_mini {

/// One entry of the feed (feed.json: {"addons":[...]}).
struct FeedAddon {
    std::string uid;
    std::string name;
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Feed {
    std::vector<FeedAddon> addons;
};

/// Row of the "installed" table.
struct InstalledAddon {
    std::string uid;
    std::string name;
    std::string version;
    std::vector<std::string> folders; ///< top-level folders the zip installed
    std::int64_t installed_ms = 0;
    std::uint64_t bytes = 0;
};

struct InstallProgress {
    std::string uid;
    std::string phase; ///< download | verify | extract | commit
    std::uint64_t received = 0;
    std::optional<std::uint64_t> total;
};

class AddonService {
public:
    AddonService(std::filesystem::path data_dir, std::string feed_url, std::vector<std::string> allowed_hosts)
        : m_data_dir(std::move(data_dir)), m_feed_url(std::move(feed_url)), m_addons_dir(m_data_dir / "AddOns") {
        std::filesystem::create_directories(m_addons_dir);
        // Startup hygiene: roll back installs a crash interrupted, drop temp dirs of dead runs.
        (void)boson::fs::InstallTransaction::recover(m_addons_dir);
        (void)boson::fs::cleanup_stale_temp_dirs("addon-mini", std::chrono::hours{24});

        boson::net::HttpClientOptions http;
        http.user_agent = L"addon-manager-mini/1.0";
        http.url_filter = [hosts = std::move(allowed_hosts)](std::string_view url) {
            const std::string host = boson::net::url_host(url);
            for (const auto& h : hosts) {
                if (h == host) return true;
            }
            return false;
        };
        http.cache = boson::net::HttpCacheOptions{.default_ttl = std::chrono::seconds{30}};
        m_http = std::make_unique<boson::net::HttpClient>(std::move(http));

        boson::db::DatabaseOptions db;
        db.path = m_data_dir / "addons.sqlite";
        auto opened = boson::db::Database::open(db);
        if (!opened) {
            throw std::runtime_error("cannot open the addon database: " + opened.error().message);
        }
        m_db = std::make_unique<boson::db::Database>(std::move(*opened));
        m_installed = std::make_unique<boson::db::Table<InstalledAddon>>(*m_db, "installed");
        if (auto r = m_installed->create(); !r) {
            throw std::runtime_error(r.error().message);
        }
    }

    [[nodiscard]] const std::filesystem::path& addons_dir() const noexcept { return m_addons_dir; }

    [[nodiscard]] Feed fetch_feed() {
        auto resp = m_http->get(m_feed_url);
        if (!resp) {
            throw std::runtime_error("feed: " + resp.error().message);
        }
        if (!resp->ok()) {
            throw std::runtime_error("feed: HTTP " + std::to_string(resp->status));
        }
        Feed feed;
        if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(feed, resp->body)) {
            throw std::runtime_error("feed: invalid JSON: " + glz::format_error(ec, resp->body));
        }
        return feed;
    }

    [[nodiscard]] std::vector<InstalledAddon> installed() {
        auto rows = m_installed->all();
        if (!rows) {
            throw std::runtime_error(rows.error().message);
        }
        return std::move(*rows);
    }

    /// Download + verify + staged install + record. on_progress is called from this thread.
    InstalledAddon install(const FeedAddon& addon, const std::function<void(const InstallProgress&)>& on_progress, std::stop_token stop = {}) {
        auto temp = boson::fs::TempDir::create("addon-mini");
        if (!temp) {
            throw std::runtime_error(temp.error().message);
        }
        const std::filesystem::path zip_path = temp->path() / "package.zip";
        boson::net::DownloadOptions dl;
        dl.expected_hash = addon.sha256; // SHA-256 verified while streaming; mismatches delete the file
        dl.progress_interval = std::chrono::milliseconds{50};
        dl.on_progress = [&](const boson::net::TransferProgress& p) {
            on_progress(InstallProgress{.uid = addon.uid, .phase = "download", .received = p.received, .total = p.total});
        };
        auto got = m_http->download(boson::net::HttpRequest{.url = addon.url, .stop = stop}, zip_path, dl);
        if (!got) {
            throw std::runtime_error("download: " + got.error().message);
        }
        on_progress(InstallProgress{.uid = addon.uid, .phase = "extract", .received = got->bytes, .total = got->bytes});

        auto reader = boson::archive::ZipReader::open(zip_path);
        if (!reader) {
            throw std::runtime_error("zip: " + reader.error().message);
        }
        auto tx = boson::fs::InstallTransaction::begin(m_addons_dir);
        if (!tx) {
            throw std::runtime_error(tx.error().message);
        }
        auto report = reader->extract_to(tx->staging_dir(), boson::archive::ExtractOptions{.stop = stop});
        if (!report) {
            throw std::runtime_error("extract: " + report.error().message);
        }
        if (auto planned = tx->replace_all_staged(); !planned) {
            throw std::runtime_error(planned.error().message);
        }
        on_progress(InstallProgress{.uid = addon.uid, .phase = "commit", .received = got->bytes, .total = got->bytes});
        if (auto committed = tx->commit(); !committed) {
            throw std::runtime_error("install: " + committed.error().message);
        }
        InstalledAddon record{
            .uid = addon.uid,
            .name = addon.name,
            .version = addon.version,
            .folders = {},
            .installed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
            .bytes = report->bytes,
        };
        for (const auto& folder : report->top_level) {
            record.folders.push_back(folder);
        }
        if (auto saved = m_installed->upsert(record); !saved) {
            throw std::runtime_error(saved.error().message);
        }
        return record;
    }

    bool uninstall(const std::string& uid) {
        auto row = m_installed->get(uid);
        if (!row || !*row) {
            return false;
        }
        auto tx = boson::fs::InstallTransaction::begin(m_addons_dir);
        if (!tx) {
            throw std::runtime_error(tx.error().message);
        }
        for (const auto& folder : (*row)->folders) {
            if (auto r = tx->remove(boson::core::utf8_to_wide(folder)); !r) {
                throw std::runtime_error(r.error().message);
            }
        }
        if (auto committed = tx->commit(); !committed) {
            throw std::runtime_error(committed.error().message);
        }
        return m_installed->remove(uid).value_or(false);
    }

private:
    std::filesystem::path m_data_dir;
    std::string m_feed_url;
    std::filesystem::path m_addons_dir;
    std::unique_ptr<boson::net::HttpClient> m_http;
    std::unique_ptr<boson::db::Database> m_db;
    std::unique_ptr<boson::db::Table<InstalledAddon>> m_installed;
};

} // namespace addon_mini
