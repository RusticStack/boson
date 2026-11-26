# Native app services

HTTP, archives, hashing, staged installs, known folders and an embedded database: the native half an app
like an addon manager (Scribe) needs, with no Node and no external runtime. Every capability exists as a
typed C++ API, and the ones a page may use are also exposed through an IPC controller (typed TS from
`boson_tsgen`) with JS helpers in `frontend/src`.

| Module | Header | Controller (JS) | JS helper | Events |
|---|---|---|---|---|
| HTTP client | `boson/net/http_client.hpp`, `http_cache.hpp`, `proxy.hpp` | `Http` | `frontend/src/net.ts` (`http`) | `http:progress`, `http:done` |
| ZIP archives | `boson/archive/zip.hpp` | (none, native only) | | |
| Hashing | `boson/crypto/hash.hpp` | (none; downloads verify hashes) | | |
| Staged installs, temp dirs, known folders | `boson/fs/install_transaction.hpp`, `known_folders.hpp` | (none, native only) | | |
| Database (SQLite, ZephyrDB) | `boson/db/database.hpp`, `table.hpp`, `kv_store.hpp` | `Db` (key/value) | `frontend/src/db.ts` (`db`) | |

Everything here blocks the calling thread (network, disk). Call it from the router's pool threads (controller
methods already run there), from `TaskRunner::spawn`, or through the sender APIs (`HttpClient::send_on`,
`Database::run_on`). Nothing creates a thread or a timer until it is used. When idle, none of it
uses CPU except the ZephyrDB backend (see below).

## net::HttpClient

WinHTTP (the shipping `HttpClient` generalised). One client owns one session, opened lazily, so connections
are reused (keep-alive; 200 sequential loopback GETs used 0 new connections).

```cpp
#include <boson/net/http_client.hpp>

boson::net::HttpClientOptions opts;
opts.retry.max_attempts = 4;                       // 5xx / 429 / transport errors, exponential backoff, Retry-After
opts.cache = boson::net::HttpCacheOptions{};       // optional LRU + TTL cache for GET 200 (honours Cache-Control)
opts.url_filter = [](std::string_view url) { return boson::net::url_host(url) == "api.example.com"; };
boson::net::HttpClient client(opts);

auto feed = client.get("https://api.example.com/feed.json");              // HttpResult<HttpResponse>
if (!feed) log(feed.error().kind, feed.error().message);

auto file = client.download({.url = "https://cdn.example.com/a.zip", .timeout = std::chrono::minutes{5}},
                            dir / "a.zip",
                            {.on_progress = [](const boson::net::TransferProgress& p) { /* p.received / p.total */ },
                             .expected_hash = "9f86d0...",                 // sha256 inferred from the length
                             .overwrite = true});

// Sender form: stop tokens from the receiver environment cancel the request.
auto s = client.send_on(pool.get_scheduler(), {.url = "https://api.example.com/x"});
```

- Methods GET / HEAD / POST / PUT / PATCH / DELETE / OPTIONS, request + default headers, request body.
- Redirects are followed by Boson, not WinHTTP (`WINHTTP_DISABLE_REDIRECTS`). That way every hop goes through
  `url_filter` and `require_https`. Other rules: 303, and 301/302 after a POST, become GET; https to http
  redirects are refused unless `allow_https_downgrade` is set; `Authorization` and `Cookie` are dropped when
  the host changes; `max_redirects` (default 10).
- `gzip` / `deflate` are decoded by WinHTTP (`WINHTTP_OPTION_DECOMPRESSION`). **Brotli is not supported**:
  WinHTTP does not decode it, so Boson never advertises `br`.
- HTTP/2 is requested (`WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`), and HTTP/3 too when `enable_http3` is set. The
  protocol actually used is in `HttpResponse::protocol`. WinHTTP only negotiates HTTP/2 over TLS.
- Timeouts:
  - WinHTTP resolve, connect and send timeouts.
  - A per-read receive timeout, enforced by a thread-pool watchdog that closes the request handle. WinHTTP's
    own receive timeout did not fire while a synchronous `WinHttpSendRequest` waited for a silent server
    (measured: 3 s instead of 400 ms).
  - An optional overall deadline per request (`HttpRequest::timeout`).
- Cancellation:
  - `HttpRequest::stop` (a `std::stop_token`), or the sender's stop token, closes the WinHTTP handle. The
    blocked call returns at once: 200 ms cancel, returned at 201 ms.
  - Backoff waits are interruptible.
  - A cancelled download deletes its `.partial` file.
- Size caps:
  - `max_body_bytes` (default 64 MiB) for `send`; `max_download_bytes` (default unlimited) for `stream` and
    `download`. Both can be overridden per request.
  - A declared `Content-Length` over the cap fails before reading. Chunked bodies are counted while streaming.
- Streaming:
  - `stream(request, sink, options)` delivers 128 KiB chunks to a callback.
  - `download(...)` writes `<dest>.partial`, preallocated when the size is known, hashes while writing
    (`hash` / `expected_hash`, constant-time compare), then renames with `MoveFileExW(WRITE_THROUGH)`.
  - Progress is throttled to `progress_interval`.
- Proxy: `ProxyMode::System` (default) uses `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` (WPAD / PAC / static IE
  settings), with loopback always direct. `Direct` and `Explicit` (`server` + `bypass`) are also available.
  `proxy.hpp` provides these helpers:
  - `read_system_proxy_config`;
  - `parse_proxy_list` (`http=a:1;https=b:2`, URL forms, IPv6);
  - `proxy_bypassed` (wildcards, `<local>`);
  - `resolve_system_proxy(url)` (PAC evaluation through `WinHttpGetProxyForUrl`).

### Http controller

```cpp
boson::ipc::HttpControllerOptions http_opts;
http_opts.allowed_hosts = {"api.example.com", "*.cdn.example.com"};   // default deny: empty = nothing
boson::ipc::HttpController http(bridge, runner, scoped_fs, http_opts);
```

```ts
import { http } from './net';
const feed = await http.json<Feed>('https://api.example.com/feed.json');
const done = await http.download(url, 'C:/Users/me/AppData/Local/MyApp/cache/a.zip', {
  expectedHash: sha256, onProgress: (p) => bar.value = p.fraction, signal: abort.signal });
```

- `fetch` returns status, headers and body (text, or base64 for binary). Bodies are capped at
  `max_fetch_bytes` (8 MiB) because they travel as JSON.
- `download` writes only to a path the `ScopedFs` grants with write access. It streams to disk on the
  `TaskRunner` and reports `http:progress` and `http:done`. `cancel({id})` stops it.
- Large payloads never go through JSON IPC: download to a scoped path, then serve the file with
  `transfer::StreamServer`, or read it with `BinaryTransport` / `Fs`.
- The host allow-list is checked for the first URL **and every redirect hop**. A redirect to a host that is not
  listed fails with `refused`.

## archive::Zip

`ZipReader`:
- Opens an archive memory-mapped (`open`) or from memory (`open_memory`).
- Supports stored and deflate (method 0 and 8), and ZIP64 (EOCD64 locator, extra fields).
- `read(entry, sink)` streams; `read_all` buffers; `extract_to(dir)` extracts into an empty staging directory.

Hostile archives are rejected at open, before anything is written:
- **Paths**:
  - `..`, absolute paths, `\\`-rooted paths, drive prefixes (`C:x`), any `:` (including NTFS alternate
    data streams);
  - control characters and `<>"|?*`, a trailing dot or space;
  - reserved device names (`CON`, `NUL`, `COM1`, ... with any extension);
  - depth over 64, invalid UTF-8;
  - names that differ only by case (they would collide on NTFS).
- **Symlinks**: Unix mode `S_IFLNK` entries are rejected (`reject_symlinks`, on by default). Extraction never
  follows reparse points: each file is created with `CREATE_NEW`, and its final path is checked to be inside the
  destination with `GetFinalPathNameByHandle`.
- **Bombs**:
  - `ZipLimits` caps the entry count (65,536), entry size (1 GiB), total size (4 GiB) and compression ratio
    (200:1 above 1 MiB);
  - archives whose entry data ranges overlap are rejected (non-recursive overlap bombs);
  - inflation stops at the declared size, so a lying header fails while inflating;
  - CRC-32 is verified, and a partially written file is deleted.
- Encrypted entries and other compression methods fail with `Unsupported`.

`ZipWriter` (stored or deflate, optional forced ZIP64) exists for tests, exports and the example.
`gzip_compress`, `deflate_raw` and `inflate_raw` are exposed for HTTP bodies and tests.

**Dependency: miniz 3.1.2** (MIT, a single C file, FetchContent with SHA-256 pinning), used only for raw
deflate/inflate and CRC-32. The ZIP container parsing is Boson's own: the code that decides what reaches the
disk is small and auditable, and it validates every offset against the mapped size.

Alternatives considered:
- libzip / minizip-ng: larger, with their own I/O layers and path handling.
- Windows Compression API: no raw deflate.
- The Shell ZIP folder (COM): slow, no limits, no streaming.
- zlib: miniz is a drop-in subset with no build system of its own.

## crypto::hash

BCrypt (CNG) via pseudo-handles (`BCRYPT_*_ALG_HANDLE`, nothing to open or close). It provides:
- MD5, SHA-1, SHA-256, SHA-384 and SHA-512;
- a streaming `Hasher` (reusable after `finish()`) and one-shot `hash`;
- `hash_file` (1 MiB sequential reads, stop token);
- `Digest::hex()` / `matches_hex()` (constant-time, case-insensitive).

Test vectors: RFC 1321 / FIPS 180 (`abc`, empty, one million `a` streamed in 997-byte chunks).

## fs: staged installs, temp dirs, known folders

```cpp
auto tx = boson::fs::InstallTransaction::begin(addons_dir);        // <root>/.boson-txn-<pid>-<id>/staging
reader->extract_to(tx->staging_dir());
tx->replace_all_staged();                                          // or replace(L"Folder") / remove(L"Folder")
auto report = tx->commit();                                        // all or nothing
```

`commit()`:
1. Writes a journal (`state: committing`, atomically).
2. For each target, renames target to `backup/` and `staging/x` to target (`MoveFileExW(WRITE_THROUGH)`, with
   short retries on sharing violations).
3. On any failure, renames everything back in reverse order. The root is left exactly as it was.
4. On success, writes `committed` and deletes the backups.

`InstallTransaction::recover(root)`, called at startup:
- rolls back journals left in `committing` (crash or power loss in the middle of a commit);
- deletes abandoned staging directories;
- skips transactions owned by a live process, matching both the pid and the process start time.

`atomic_replace(staged, target)` does the same for a single target. `remove_tree` never follows junctions or
symlinks and clears read-only attributes.

`known_folder(KnownFolder::Documents | Downloads | SavedGames | LocalAppData | ...)` uses
`SHGetKnownFolderPath`, so a Documents folder redirected to OneDrive is returned as the redirected path.
`onedrive_roots()` reads the `OneDrive*` environment variables and `HKCU\Software\Microsoft\OneDrive\Accounts`.
`document_folder_candidates()` lists the known Documents folder plus `<OneDrive root>\Documents` (an ESO
addon manager has to look in both).

`TempDir::create(prefix)` creates `%TEMP%\<prefix>-<pid>-<random>` and deletes it when destroyed.
`cleanup_stale_temp_dirs(prefix, max_age)` deletes leftovers of dead processes at startup.

## db: one API, two backends

```cpp
#include <boson/db/table.hpp>

struct Addon { std::string uid; std::string name; std::int64_t version = 0; std::vector<std::string> tags; };

auto db = boson::db::Database::open({.path = data_dir / "app.sqlite"});   // backend: default_backend()
boson::db::Table<Addon> addons(*db, "addons");                              // primary key: first field
addons.create();                                                            // CREATE TABLE IF NOT EXISTS (+ new columns)
addons.upsert({.uid = "lam2", .name = "LibAddonMenu", .version = 34});
auto newest = addons.where("version >= $1 ORDER BY version DESC LIMIT 5", std::int64_t{30});
db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> { /* commit on success */ return {}; });
auto n = db->query_value<std::int64_t>("SELECT COUNT(*) FROM addons");      // prepared + cached
boson::db::KvStore kv(*db); kv.create(); kv.set_as("prefs", "main", prefs);
```

- Row mapping uses glaze reflection, so there is no binding code. Column types by member type:
  - integers become `INTEGER` / `BIGINT`;
  - floats become `REAL` / `DOUBLE PRECISION`;
  - `bool` becomes an integer (SQLite) or `BOOLEAN` (ZephyrDB);
  - strings become `TEXT`;
  - `Blob` becomes `BLOB` / `BYTEA`;
  - `std::optional` is nullable;
  - anything else (vectors, nested structs) is stored as JSON text.
- `$N` placeholders work on both backends.
- Prepared statements are cached per connection (LRU, 64).
- `transaction()` commits on success and rolls back on an error result or an exception. Nested transactions use
  savepoints (SQLite) or roll back the outer transaction (ZephyrDB).
- `Database` serialises access to one connection with a mutex. `run_on(scheduler, fn)` is the sender form.
- Backends are compile-time options, both ON by default: `BOSON_DB_SQLITE` and `BOSON_DB_ZEPHYR`.
  `DatabaseOptions::backend` picks one per database.

**SQLite 3.54.1** (amalgamation, FetchContent with SHA-256 pinning, `SQLITE_THREADSAFE=2`, `SQLITE_DQS=0`).
Why SQLite:
- public domain and the most deployed database engine;
- one file, crash-safe WAL;
- an online backup API.
- `redb` (what Scribe uses) is Rust-only.
- LMDB has no SQL and needs a fixed map size.

Defaults: WAL, `synchronous=NORMAL`, `busy_timeout` 5 s, foreign keys on, `temp_store=MEMORY`. `backup_to` uses
`sqlite3_backup`; `checkpoint` is `wal_checkpoint(TRUNCATE)`.

### Db controller

```cpp
boson::ipc::DbController dbc(shared_db, {.namespaces = {"settings", "cache"}, .read_only_namespaces = {"catalog"}});
```
```ts
import { db } from './db';
const settings = db.namespace('settings');
await settings.set('theme', { dark: true });
const theme = await settings.get<{ dark: boolean }>('theme');
```

The controller is key/value only (`get`, `set`, `remove`, `keys` / `entries` by prefix, `clear`, `count`,
`info`); the page never sees SQL.
- Namespaces are default-deny. A namespace can also be read-only.
- Values must be valid JSON and fit the size caps: 1 MiB per value, 512 bytes per key, 10,000 entries per list.

## ZephyrDB backend

ZephyrDB (https://github.com/KelpHect/ZephyrDB) is pinned with FetchContent at
`c50eea79fee693df1931c471819016dacf43a09e`:
- `ZEPHYR_BUILD_TESTS`, `ZEPHYR_BUILD_BENCHMARKS` and `ZEPHYR_BUILD_SERVER` are OFF, and the target is
  `EXCLUDE_FROM_ALL`.
- Boson links `zephyrdb_core` statically and talks to it only through `zephyr/c_abi.h`.
- Its warnings are confined to its own target: `/wd4702` on `zephyrdb_core` only. Boson's flags are unchanged.

`DatabaseOptions` settings that only apply to ZephyrDB:
- `path` is a directory (`zephyr.snap` + `zephyr.wal`);
- `encryption_key` uses `zephyr_db_open_encrypted`;
- `zephyr_wal_idle_flush` (default 1 s instead of ZephyrDB's 2 ms);
- `zephyr_compactor` (default off);
- `zephyr_checkpoint_on_close` (default on).

Boson adds what the engine lacks:
- an exclusive `boson.lock` per directory, plus an in-process registry. ZephyrDB does not lock its files, and two
  engines replaying one WAL corrupt it;
- upsert emulation (UPDATE, then INSERT when no row changed), because ZephyrDB has no `ON CONFLICT`;
- inline `LIMIT`, because it accepts no `LIMIT $n`.

### Findings (honest)

| Topic | Result |
|---|---|
| **Durability bug (worked around)** | In `c50eea7`, a parameterised `INSERT` / `UPDATE` / `DELETE` that misses the engine's PK fast path is applied in memory but **never written to the WAL**. That covers every statement inside `BEGIN..COMMIT`, text primary keys, multi-column `SET` and `DELETE`. `executor.cpp` `exec_stmt` logs only when `params.empty()`. Data committed this way was lost on a kill: 0 of 100 rows survived. Boson's workaround: on a persistent database, writes run with their parameters inlined as escaped SQL literals, which ZephyrDB logs verbatim. After the workaround, 100 of 100 survived. Cost: the writes lose the prepared fast path. Values that cannot be inlined (NaN/inf, INT64_MIN, text or blobs containing NUL bytes) are rejected rather than silently written in a non-durable way. |
| Durability model | `COMMIT` waits for the WAL flush and fsync, so committed transactions survive a process kill (tested). A single autocommit statement is acknowledged before it is written (group commit every `wal_flush_ms`). A kill loses the last window: with Boson's 1 s setting, about 0.5–1 s of autocommit writes. After waiting 1.5 s, 822 to 1,336 autocommit rows survived the kill (tested). Survival of an OS crash or power loss was **not tested**. `Synchronous::Full` is refused at open with a message pointing to transactions. SQLite with WAL and `NORMAL` loses nothing on a process kill (7,042 rows survived the same test) and may lose the last commits on power loss. |
| Crash recovery | A child process killed during writes, then reopened: committed data intact, the engine writable (`test_native_services`). |
| Encryption | Works. The plaintext value does not appear in `zephyr.snap` or `zephyr.wal`, a wrong key fails to open, and the right key reads the data back. SQLite has no encryption in this build; an `encryption_key` is refused. |
| SQL coverage for Scribe | Scribe stores its cache and install records in `redb` key/value tables (about 7,000 addons). `Table<T>` and `KvStore` cover that on both backends. ZephyrDB lacks `ON CONFLICT`, `ALTER TABLE` (no automatic new columns), `LIKE`, `IN (...)`, savepoints and `LIMIT $n`. Boson's SQL generation avoids all of them. |
| Transactions | `BEGIN` clones **every table** (`txn_backup_ = clone_data`), so each transaction costs O(total data): a one-row transaction took **17.8 ms** with 100k rows (SQLite: 13 µs). `in_txn_` is engine-global, so transactions from different threads collide; Boson serialises them through its connection mutex. |
| Idle CPU | The RCU reaper is hard-coded to wake every 2 ms (`reaper_.start(2)`, not configurable through the C ABI). The WAL guardian wakes every `wal_flush_ms`, and the compactor every 5 s when enabled. Measured over 10 s idle: **72 context switches/s, 31 ms CPU** with Boson's settings; 144/s and 62 ms with ZephyrDB defaults (2 ms flush, compactor on); 211/s for an in-memory engine. Baseline (no database): 0.7/s, 0 ms; SQLite open: 0.3/s, 0 ms. This breaks Boson's zero-idle-CPU rule; it can only be fixed upstream. |
| Memory | 100k rows (4 columns): **+88 MiB private memory** (the whole database lives in RAM) vs **+3.7 MiB** for SQLite. |
| Range scans | `WHERE id >= $1 AND id < $2` (100 rows out of 100k) took **9.0 ms**, which looks like a full scan, vs 5 µs on SQLite. |
| Binary size (Release, `boson_db_bench.exe`) | Both backends: 1,998,848 B. SQLite only: 1,241,600 B. ZephyrDB only: 900,608 B. So ZephyrDB adds about **740 KiB** and SQLite about **1.05 MiB**. The `zephyrdb_core.lib` static library is 79 MB on disk; the linker keeps only what is used. ZephyrDB's Release flags add `/arch:AVX2` to its own target, so a ZephyrDB build needs an AVX2 CPU. |
| Build | ZephyrDB always defines a shared-library target too; `EXCLUDE_FROM_ALL` keeps it from being built. |
| Other | There is no `last_insert_rowid` (`Connection::last_insert_rowid()` returns 0). The WAL guardian opens and closes the WAL file for every batch. |

### Benchmark (Release, 100k rows, through Boson's API)

`tests/bench_db.cpp` (`boson_db_bench [rows] [--idle-seconds=N]`). Single run on a desktop running other
work. Treat the numbers as an order of magnitude.

| Operation | SQLite 3.54.1 | ZephyrDB c50eea7 |
|---|---|---|
| Open (empty) | 4.0 ms | 1.0 ms |
| Bulk insert 100k (1 transaction) | 35 ms (2.8 M rows/s) | 3,229 ms (31 k rows/s)¹ |
| Autocommit insert | 21.5 µs/row | 26.9 µs/row¹ |
| Point select (`query_value`) | 1.52 µs | **0.27 µs** |
| Point select (`Table::get`, struct) | 1.56 µs | **0.38 µs** |
| Range select (100 rows) | **5.0 µs** | 9,027 µs |
| Update by PK, autocommit | 15.1 µs | 21.9 µs¹ |
| Update by PK, inside a transaction | 1.4 µs | 25.9 µs¹ |
| Small transaction (1 update + commit) | **13 µs** | 17,790 µs² |
| Mixed: 4 readers + 1 writer, 3 s | 192 k reads/s + 30 k writes/s | 179 k reads/s + 37 k writes/s |
| Checkpoint / close | 0.7 ms / 0.7 ms | 37 ms / 23 ms |
| Reopen with 101k rows | **0.7 ms** | 119 ms (loads everything) |
| File size after checkpoint | 3.0 MiB | 4.1 MiB |
| Private memory for the data | 3.7 MiB | 88 MiB |
| Idle, open database | 0 ms CPU, 0.3 switches/s | 31 ms CPU / 10 s, 72 switches/s |

¹ Includes Boson's durability workaround (parameters inlined as literals, so every write is parsed).
² `BEGIN` copies every table.

Mixed load goes through one `Database` (one connection behind a mutex), so readers are serialised on both
backends.

**Recommendation: SQLite stays the default** (`default_backend()` returns SQLite when it is linked). ZephyrDB is
4-6 times faster on point reads. It is much slower on writes, transactions, range scans and reopen. It uses
about 24 times the memory, wakes the CPU about 72 times a second when idle, and needed a workaround to make
committed transactions durable at this commit. Use it per database (`.backend = Backend::Zephyr`) for small
read-heavy data that has to be encrypted, or build with `-DBOSON_DB_ZEPHYR=OFF` to drop it.

## Example: examples/addon_manager_mini

`boson_addon_manager_mini` is the Scribe pipeline in about 560 lines:
- An in-process loopback server stands in for the addon site (`feed.json` plus zips built at startup; no
  scraping).
- `AddonService`:
  - fetches the feed (cached);
  - downloads each zip with progress and SHA-256 verification;
  - validates and extracts it into an `InstallTransaction`, then commits;
  - stores `InstalledAddon` rows in SQLite, and uninstalls through a transaction as well.
- `AddonController` exposes it to a minimal page served from memory, with `addons:progress` / `addons:done` events.
- One feed entry has a wrong hash on purpose and is refused.
- `boson_addon_manager_mini --selftest` runs the pipeline headless (registered with CTest).

## Measured hot paths

Release, from `boson_native_services_tests` (loopback, Windows 11 26300):

| Path | Result |
|---|---|
| SHA-256 of an 8 MiB file (`hash_file`) | 4.9 ms (1.6 GiB/s) |
| Download 5 MiB over loopback + SHA-256 + rename | 13.5 ms (371 MiB/s) |
| First GET (session + connection) | 5.1 ms |
| Keep-alive GET through the test server | 108 µs per request (the server is a naive thread per connection) |
| Cancel / deadline / receive timeout latency | +1 to +7 ms over the requested time |
| ZIP central directory, 2,000 entries | 1.2 ms |
| Extract 2,000 files (4 MB) | 753 ms, about 0.38 ms per file (create, preallocate, write, verify the final path, set the time). Not profiled; real-time antivirus scanning on this machine is likely a large share. |
| Interrupted commit: crash child + `recover()` | 24 ms |
| Locked target during `commit()` | fails after about 700 ms of sharing-violation retries, then rolls back |

## Verification

What was run (Debug `/W4` build, warning-free; plus the Release builds above):
- `boson_native_services_tests`: 541 checks, 0 failures.
  - Hashing vectors.
  - ZIP:
    - stored, deflate and forced ZIP64 round trips;
    - 15 unsafe names, a symlink entry, case-duplicate names;
    - a declared-size bomb (partial file deleted), a 64 MiB ratio bomb, the entry-count limit;
    - a CRC mismatch, truncated and junk archives.
  - Staged install:
    - create, replace, remove;
    - rollback injected at every rename position, a locked target;
    - a child process killed between renames, followed by `recover()`; a live transaction skipped;
    - temp cleanup, known folders.
  - Proxy parsing and bypass rules.
  - HTTP against a loopback server:
    - headers, keep-alive reuse, HEAD / POST / PUT;
    - redirect chains, loops, 303, the filter on a redirect hop, `require_https`;
    - gzip; chunked bodies;
    - the deadline and the receive timeout;
    - 503 retries with Retry-After; no retry for POST; connection resets with retries;
    - cancellation mid-body;
    - the Content-Length cap and the chunked cap;
    - the cache (TTL, no-store, LRU);
    - downloads with progress, hash, mismatch cleanup and 404;
    - an explicit proxy, using the loopback server as the proxy for an `.invalid` host;
    - senders (`sync_wait`, stop from `when_all`).
  - Controllers through the `Router`:
    - Http: allow-list, a redirect to a blocked host, base64 bodies, download to a scoped path, a path outside
      the scope refused, cancel;
    - Db: namespaces, read-only namespaces, JSON validation.
  - The same database suite on **both** backends: CRUD, JSON and optional columns, upsert, `where`, indexes,
    transactions (error, exception, nested), `KvStore`, 4 threads of concurrent upserts and reads, the sender, a
    checkpoint, a child-process crash during writes; ZephyrDB encryption and its single-opener lock.
- The full CTest suite: 15 of 15 passed (`compat_typecheck` is disabled, as before).
- `boson_tsgen` was re-run; `tsc --noEmit` over `frontend/src` (including `net.ts` and `db.ts`) is clean.

Not verified:
- The example's **window was not opened** in this session, to avoid taking focus from other work. Only
  `--selftest` (the same service code, headless) ran. The page and its event handling are untested.
- `net.ts` / `db.ts` were type-checked but not run in a WebView.
- **HTTP/2 and HTTP/3 were not exercised**: the loopback server is plain HTTP/1.1, and WinHTTP negotiates h2 only
  over TLS. TLS paths (certificate errors to `Tls`) were not tested.
- PAC / WPAD resolution and `resolve_system_proxy` only ran against this machine's settings (auto-detect, no
  PAC). HTTPS through an explicit proxy (CONNECT) was not tested.
- OneDrive redirection: this machine has no OneDrive, so `onedrive_roots()` returned nothing. The registry and
  environment-variable code paths were not exercised with real data.
- ZIP64 was tested with forced ZIP64 records on small archives, not with files over 4 GiB.
- Durability after an OS crash or power loss was not tested on either backend; only process kills were.
- Benchmarks are single runs on a busy desktop.
- There is no `compat/electron.ts` mapping for `net.request`, `better-sqlite3` or similar APIs.
