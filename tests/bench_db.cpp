// boson::db backend benchmark: the same workload through Boson's typed API on every linked backend.
// Build Release and run: boson_db_bench [rows=100000] [--idle-seconds=10]
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <boson/db/database.hpp>
#include <boson/db/table.hpp>
#include <boson/fs/install_transaction.hpp>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace bench_records {
struct BenchRow {
    std::int64_t id = 0;
    std::string name;
    std::int64_t score = 0;
    double weight = 0.0;
};
} // namespace bench_records
using bench_records::BenchRow;

namespace {

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::uint64_t dir_bytes(const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
        std::uint64_t total = fs::file_size(p, ec);
        for (const char* suffix : {"-wal", "-shm"}) {
            fs::path side = p;
            side += suffix;
            if (fs::exists(side, ec)) total += fs::file_size(side, ec);
        }
        return total;
    }
    std::uint64_t total = 0;
    for (const auto& e : fs::recursive_directory_iterator(p, ec)) {
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    }
    return total;
}

std::uint64_t private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

double process_cpu_ms() {
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const auto v = [](FILETIME f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(v(k) + v(u)) / 10'000.0;
}

/// Sum of context switches of this process's threads (NtQuerySystemInformation, SystemProcessInformation).
std::uint64_t context_switches() {
    using NtQuery = LONG(WINAPI*)(int, void*, ULONG, ULONG*);
    static const auto query = reinterpret_cast<NtQuery>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (query == nullptr) return 0;
    std::vector<std::byte> buffer(4u << 20);
    ULONG needed = 0;
    while (query(5, buffer.data(), static_cast<ULONG>(buffer.size()), &needed) == static_cast<LONG>(0xC0000004L)) {
        buffer.resize(needed + (1u << 20));
    }
    const auto pid = static_cast<std::uintptr_t>(GetCurrentProcessId());
    std::size_t offset = 0;
    while (true) {
        const std::byte* entry = buffer.data() + offset;
        ULONG next = 0;
        ULONG threads = 0;
        std::uintptr_t entry_pid = 0;
        std::memcpy(&next, entry, sizeof(next));
        std::memcpy(&threads, entry + 4, sizeof(threads));
        std::memcpy(&entry_pid, entry + 0x50, sizeof(entry_pid));
        if (entry_pid == pid) {
            std::uint64_t total = 0;
            for (ULONG t = 0; t < threads; ++t) {
                ULONG cs = 0;
                std::memcpy(&cs, entry + 0x100 + t * 0x50 + 0x40, sizeof(cs)); // SYSTEM_THREAD_INFORMATION.ContextSwitches
                total += cs;
            }
            return total;
        }
        if (next == 0) return 0;
        offset += next;
    }
}

void row(std::string_view label, double value, std::string_view unit) {
    std::cout << "  " << label << ": " << value << ' ' << unit << '\n';
}

void idle_probe(std::string_view label, int seconds) {
    const double cpu0 = process_cpu_ms();
    const std::uint64_t cs0 = context_switches();
    std::this_thread::sleep_for(std::chrono::seconds{seconds});
    const double cpu = process_cpu_ms() - cpu0;
    const double cs = static_cast<double>(context_switches() - cs0);
    std::cout << "  idle " << label << " (" << seconds << " s): CPU " << cpu << " ms, " << cs / seconds << " context switches/s\n";
}

void run(boson::db::Backend backend, int rows, int idle_seconds, const fs::path& base) {
    const std::string name(boson::db::to_string(backend));
    std::cout << "\n[" << name << "] " << boson::db::backend_version(backend) << '\n';
    boson::db::DatabaseOptions opts;
    opts.backend = backend;
    opts.path = backend == boson::db::Backend::Sqlite ? base / "bench.sqlite" : base / "zephyr";
    const std::uint64_t mem0 = private_bytes();

    auto t0 = Clock::now();
    auto db = boson::db::Database::open(opts);
    if (!db) {
        std::cout << "  open failed: " << db.error().message << '\n';
        return;
    }
    row("open (empty)", ms_since(t0), "ms");
    boson::db::Table<BenchRow> table(*db, "bench");
    (void)table.create();

    std::mt19937_64 rng(42);
    t0 = Clock::now();
    auto ins = db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> {
        for (int i = 0; i < rows; ++i) {
            auto r = c.execute("INSERT INTO bench (id, name, score, weight) VALUES ($1, $2, $3, $4)", std::int64_t{i},
                               std::string("addon-") + std::to_string(i), static_cast<std::int64_t>(rng() % 1000), static_cast<double>(i) * 0.5);
            if (!r) return std::unexpected(r.error());
        }
        return {};
    });
    double ms = ms_since(t0);
    if (!ins) std::cout << "  insert failed: " << ins.error().message << '\n';
    row("bulk insert (1 transaction)", ms, "ms");
    row("bulk insert rate", rows / (ms / 1000.0), "rows/s");

    t0 = Clock::now();
    for (int i = 0; i < 1000; ++i) {
        (void)db->execute("INSERT INTO bench (id, name, score, weight) VALUES ($1, $2, $3, $4)", std::int64_t{rows + i}, std::string("auto"), std::int64_t{1}, 1.0);
    }
    row("autocommit insert", ms_since(t0) * 1000.0 / 1000, "us/row");

    constexpr int kPoint = 50'000;
    std::int64_t checksum = 0;
    t0 = Clock::now();
    for (int i = 0; i < kPoint; ++i) {
        auto v = db->query_value<std::int64_t>("SELECT score FROM bench WHERE id = $1", static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rows)));
        if (v && *v) checksum += **v;
    }
    row("point select (query_value)", ms_since(t0) * 1000.0 / kPoint, "us/op");
    t0 = Clock::now();
    for (int i = 0; i < kPoint; ++i) {
        auto r = table.get(static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rows)));
        if (r && *r) checksum += (*r)->score;
    }
    row("point select (Table::get -> struct)", ms_since(t0) * 1000.0 / kPoint, "us/op");

    constexpr int kRange = 2000;
    std::int64_t seen = 0;
    t0 = Clock::now();
    for (int i = 0; i < kRange; ++i) {
        const auto lo = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rows - 100));
        (void)db->query("SELECT id, score FROM bench WHERE id >= $1 AND id < $2", [&](const boson::db::Statement& s) {
            seen += s.column_int64(1);
            return true;
        }, lo, lo + 100);
    }
    row("range select (100 rows)", ms_since(t0) * 1000.0 / kRange, "us/op");

    constexpr int kUpd = 20'000;
    t0 = Clock::now();
    for (int i = 0; i < kUpd; ++i) {
        (void)db->execute("UPDATE bench SET score = $1 WHERE id = $2", static_cast<std::int64_t>(i), static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rows)));
    }
    row("update by pk (autocommit)", ms_since(t0) * 1000.0 / kUpd, "us/op");
    t0 = Clock::now();
    (void)db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> {
        for (int i = 0; i < kUpd; ++i) {
            (void)c.execute("UPDATE bench SET score = $1 WHERE id = $2", static_cast<std::int64_t>(i), static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rows)));
        }
        return {};
    });
    row("update by pk (1 transaction)", ms_since(t0) * 1000.0 / kUpd, "us/op");
    t0 = Clock::now();
    for (int i = 0; i < 200; ++i) {
        (void)db->transaction([&](boson::db::Connection& c) -> boson::db::DbResult<void> {
            return c.execute("UPDATE bench SET score = $1 WHERE id = $2", std::int64_t{i}, std::int64_t{i}).transform([](std::int64_t) {});
        });
    }
    row("small transaction (1 update + commit)", ms_since(t0) * 1000.0 / 200, "us/op");

    {
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> writes{0};
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                std::mt19937_64 r(static_cast<std::uint64_t>(t));
                while (!stop.load(std::memory_order_relaxed)) {
                    (void)db->query_value<std::int64_t>("SELECT score FROM bench WHERE id = $1", static_cast<std::int64_t>(r() % static_cast<std::uint64_t>(rows)));
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        threads.emplace_back([&]() {
            std::mt19937_64 r(99);
            while (!stop.load(std::memory_order_relaxed)) {
                (void)db->execute("UPDATE bench SET score = $1 WHERE id = $2", std::int64_t{7}, static_cast<std::int64_t>(r() % static_cast<std::uint64_t>(rows)));
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::this_thread::sleep_for(std::chrono::seconds{3});
        stop = true;
        threads.clear();
        row("mixed 4 readers + 1 writer: reads", static_cast<double>(reads.load()) / 3.0, "ops/s");
        row("mixed 4 readers + 1 writer: writes", static_cast<double>(writes.load()) / 3.0, "ops/s");
    }
    row("private memory after load", static_cast<double>(private_bytes() - mem0) / (1024.0 * 1024.0), "MiB");
    if (idle_seconds > 0) {
        idle_probe(name + " open, Boson defaults", idle_seconds);
    }
    t0 = Clock::now();
    (void)db->checkpoint();
    row("checkpoint", ms_since(t0), "ms");
    t0 = Clock::now();
    db = std::unexpected(boson::db::DbError{});
    row("close", ms_since(t0), "ms");
    row("on-disk size", static_cast<double>(dir_bytes(opts.path)) / (1024.0 * 1024.0), "MiB");

    t0 = Clock::now();
    auto again = boson::db::Database::open(opts);
    row("reopen with data", ms_since(t0), "ms");
    if (again) {
        auto n = again->query_value<std::int64_t>("SELECT COUNT(*) FROM bench");
        std::cout << "  rows after reopen: " << (n && *n ? **n : -1) << " (checksum " << (checksum + seen) % 1000 << ")\n";
    }
    again = std::unexpected(boson::db::DbError{});

    if (backend == boson::db::Backend::Zephyr && idle_seconds > 0) {
        boson::db::DatabaseOptions zdef = opts;
        zdef.path = base / "zephyr-defaults";
        zdef.zephyr_wal_idle_flush = std::chrono::milliseconds{2};
        zdef.zephyr_compactor = true;
        auto d = boson::db::Database::open(zdef);
        if (d) {
            (void)d->exec("CREATE TABLE t (id BIGINT PRIMARY KEY)");
            idle_probe("zephyr, ZephyrDB defaults (wal_flush_ms=2, compactor on)", idle_seconds);
        }
        boson::db::DatabaseOptions mem;
        mem.backend = backend;
        auto m = boson::db::Database::open(mem);
        if (m) {
            idle_probe("zephyr in-memory (no WAL)", idle_seconds);
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int rows = 100'000;
    int idle = 10;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a.starts_with(L"--idle-seconds=")) idle = std::stoi(std::wstring(a.substr(15)));
        else rows = std::stoi(std::wstring(a));
    }
    const fs::path base = fs::temp_directory_path() / (L"boson-db-bench-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(base);
    std::cout << "rows=" << rows << '\n';
    if (idle > 0) idle_probe("baseline (no database open)", idle);
    for (const auto b : {boson::db::Backend::Sqlite, boson::db::Backend::Zephyr}) {
        if (boson::db::backend_available(b)) run(b, rows, idle, base);
    }
    (void)boson::fs::remove_tree(base);
    return 0;
}
