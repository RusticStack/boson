// Persistent settings store tests: missing / corrupt / recovered files, normalisation, change detection,
// restart keys, listeners, concurrent updates, a real crash during save (child process killed between the
// temp write and ReplaceFileW), std::execution senders, the Store IPC controller, TypeScript generation and
// (when a WebView2 runtime is installed) the controller end to end through Bridge + capabilities.
// Everything lives in a unique temp folder that is removed at the end.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glaze/glaze.hpp>
#include <stdexec/execution.hpp>

#include <boson/boson.hpp>
#include <boson/ipc/controllers/store_controller.hpp>
#include <boson/store/store.hpp>

namespace stt {

struct Nested {
    int level = 1;
    std::string label = "x";
};

struct TestSettings {
    std::string screenshare_mode = "auto";
    bool hardware_acceleration = true;
    std::string search_engine = "duckduckgo";
    int tab_sleep_minutes = 15;
    std::vector<std::string> browser_args;
    Nested nested;
    std::uint64_t counter = 0;

    static constexpr std::array<std::string_view, 2> boson_restart_keys{"hardware_acceleration", "browser_args"};
};

// Found by ADL: runs after every load and change.
void normalize(TestSettings& s) {
    s.tab_sleep_minutes = std::clamp(s.tab_sleep_minutes, 0, 1440);
    if (s.screenshare_mode != "auto" && s.screenshare_mode != "window" && s.screenshare_mode != "screen") {
        s.screenshare_mode = "auto";
    }
}

} // namespace stt

namespace {

namespace fs = std::filesystem;
namespace bs = boson::store;
using Settings = stt::TestSettings;
using SettingsStore = bs::Store<Settings>;

constexpr DWORD kCrashExitCode = 0xC0DE;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_store.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define STORE_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

template <typename S>
    requires std::convertible_to<const S&, std::string_view>
bool contains(const S& haystack, std::string_view needle) {
    return std::string_view(haystack).find(needle) != std::string_view::npos;
}

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && contains(*haystack, needle);
}

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_all(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::vector<fs::path> files_with(const fs::path& dir, std::wstring_view needle) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().wstring().find(needle) != std::wstring::npos) {
            out.push_back(it->path());
        }
    }
    return out;
}

bool same_keys(const std::vector<std::string>& keys, std::initializer_list<std::string_view> expected) {
    if (keys.size() != expected.size()) {
        return false;
    }
    std::size_t i = 0;
    for (const auto key : expected) {
        if (keys[i++] != key) {
            return false;
        }
    }
    return true;
}

// ---- missing file -------------------------------------------------------------------------------------
void test_missing(const fs::path& base) {
    step("missing file: defaults, no file until the first change, parent folders created");
    const fs::path path = base / L"missing" / L"deep" / L"settings.json";
    {
        SettingsStore store({.path = path});
        STORE_CHECK(store.load_report().status == bs::LoadStatus::Missing);
        STORE_CHECK(store.get().search_engine == "duckduckgo");
        STORE_CHECK(store.get().tab_sleep_minutes == 15);
        STORE_CHECK(store.revision() == 0);
        STORE_CHECK(!fs::exists(path));
        const auto change = store.update([](Settings& s) { s.search_engine = "kagi"; });
        STORE_CHECK(change.has_value() && change->changed && change->saved && change->revision == 1);
        STORE_CHECK(fs::exists(path));
        STORE_CHECK(contains(read_all(path), "\"search_engine\": \"kagi\""));
        STORE_CHECK(files_with(path.parent_path(), L".tmp").empty());
    }
    {
        SettingsStore reopened({.path = path});
        STORE_CHECK(reopened.load_report().status == bs::LoadStatus::Loaded);
        STORE_CHECK(reopened.get().search_engine == "kagi");
    }
    const fs::path created = base / L"create" / L"settings.json";
    SettingsStore with_create({.path = created, .create_if_missing = true});
    STORE_CHECK(with_create.load_report().status == bs::LoadStatus::Missing);
    STORE_CHECK(fs::exists(created) && contains(read_all(created), "\"tab_sleep_minutes\": 15"));

    SettingsStore memory{};
    STORE_CHECK(memory.load_report().status == bs::LoadStatus::InMemory);
    const auto mem_change = memory.update([](Settings& s) { s.counter = 5; });
    STORE_CHECK(mem_change && mem_change->changed && !mem_change->saved && memory.get().counter == 5);
}

// ---- corrupt files ------------------------------------------------------------------------------------
void test_corrupt(const fs::path& base) {
    step("corrupt file: backed up, defaults used, original bytes preserved");
    const fs::path dir = base / L"corrupt";
    const fs::path path = dir / L"settings.json";
    const std::string garbage = "{ \"search_engine\": \"bing\", oops";
    write_all(path, garbage);
    {
        SettingsStore store({.path = path});
        const auto report = store.load_report();
        STORE_CHECK(report.status == bs::LoadStatus::Corrupt);
        STORE_CHECK(!report.message.empty());
        STORE_CHECK(store.get().search_engine == "duckduckgo");
        STORE_CHECK(!report.corrupt_backup.empty() && fs::exists(report.corrupt_backup));
        STORE_CHECK(read_all(report.corrupt_backup) == garbage);
        STORE_CHECK(!fs::exists(path)); // moved aside, the next save writes a fresh file
        STORE_CHECK(store.update([](Settings& s) { s.counter = 1; }).has_value());
        STORE_CHECK(contains(read_all(path), "\"counter\": 1"));
    }

    step("empty, non-UTF-8, oversized and wrong-typed files are corrupt too");
    for (const std::string_view bad : {std::string_view{""}, std::string_view{"   \r\n"}, std::string_view{"{\"search_engine\":\"\xC3\x28\"}"},
             std::string_view{"{\"tab_sleep_minutes\":\"many\"}"}, std::string_view{"[1,2,3]"}}) {
        const fs::path p = dir / L"each.json";
        write_all(p, bad);
        SettingsStore store({.path = p, .max_corrupt_backups = 0});
        STORE_CHECK(store.load_report().status == bs::LoadStatus::Corrupt);
        STORE_CHECK(store.get().tab_sleep_minutes == 15);
        STORE_CHECK(!fs::exists(p)); // max_corrupt_backups = 0: deleted, not kept
    }
    {
        const fs::path p = dir / L"big.json";
        write_all(p, "{\"search_engine\":\"" + std::string(2048, 'a') + "\"}");
        SettingsStore store({.path = p, .max_file_bytes = 1024});
        STORE_CHECK(store.load_report().status == bs::LoadStatus::Corrupt);
    }

    step("corrupt backups are capped at max_corrupt_backups");
    const fs::path capped = base / L"capped" / L"settings.json";
    for (int i = 0; i < 5; ++i) {
        write_all(capped, "not json " + std::to_string(i));
        SettingsStore store({.path = capped, .max_corrupt_backups = 2});
        STORE_CHECK(store.load_report().status == bs::LoadStatus::Corrupt);
        Sleep(2); // distinct millisecond timestamps
    }
    const auto backups = files_with(capped.parent_path(), L".corrupt-");
    STORE_CHECK(backups.size() == 2);
    bool newest_kept = false;
    for (const auto& b : backups) {
        newest_kept = newest_kept || read_all(b) == "not json 4";
    }
    STORE_CHECK(newest_kept);

    step("corrupt file with a good .bak: the previous save is recovered");
    const fs::path rec = base / L"recover" / L"settings.json";
    {
        SettingsStore store({.path = rec});
        STORE_CHECK(store.update([](Settings& s) { s.search_engine = "first"; }).has_value());
        STORE_CHECK(store.update([](Settings& s) { s.search_engine = "second"; }).has_value());
        STORE_CHECK(store.update([](Settings& s) { s.search_engine = "third"; }).has_value());
    }
    STORE_CHECK(contains(read_all(bs::detail::backup_path_of(rec)), "second"));
    write_all(rec, "\x00\x01garbage");
    {
        SettingsStore store({.path = rec});
        STORE_CHECK(store.load_report().status == bs::LoadStatus::RecoveredFromBackup);
        STORE_CHECK(store.get().search_engine == "second");
    }
}

// ---- schema tolerance and normalize -----------------------------------------------------------------
void test_schema_and_normalize(const fs::path& base) {
    step("unknown keys ignored, missing keys default, normalize() on load and on change");
    const fs::path path = base / L"schema" / L"settings.json";
    write_all(path, "\xEF\xBB\xBF{\"search_engine\":\"brave\",\"removed_in_v2\":42,\"tab_sleep_minutes\":99999,"
                    "\"screenshare_mode\":\"bogus\",\"nested\":{\"label\":\"y\"}}");
    {
        SettingsStore store({.path = path});
        STORE_CHECK(store.load_report().status == bs::LoadStatus::Loaded);
        const Settings s = store.get();
        STORE_CHECK(s.search_engine == "brave");
        STORE_CHECK(s.tab_sleep_minutes == 1440);    // clamped by normalize
        STORE_CHECK(s.screenshare_mode == "auto");   // fixed by normalize
        STORE_CHECK(s.hardware_acceleration);        // default
        STORE_CHECK(s.nested.level == 1 && s.nested.label == "y");
        const auto change = store.update([](Settings& v) { v.tab_sleep_minutes = -5; });
        STORE_CHECK(change && store.get().tab_sleep_minutes == 0);
    }
    {
        write_all(path, "{\"search_engine\":\"brave\",\"removed_in_v2\":42}");
        SettingsStore strict({.path = path, .ignore_unknown_keys = false, .max_corrupt_backups = 0});
        STORE_CHECK(strict.load_report().status == bs::LoadStatus::Corrupt);
    }
    {
        // An explicit normalizer replaces the ADL one.
        SettingsStore custom({}, [](Settings& s) { s.search_engine = "forced"; });
        STORE_CHECK(custom.get().search_engine == "forced");
    }
    {
        SettingsStore warned({.restart_keys = {"search_engine", "no_such_key"}});
        STORE_CHECK(warned.is_restart_key("search_engine"));
        STORE_CHECK(warned.is_restart_key("hardware_acceleration")); // from Settings::boson_restart_keys
        STORE_CHECK(!warned.is_restart_key("no_such_key"));
        STORE_CHECK(warned.load_report().warnings.size() == 1);
    }
}

// ---- change detection, restart keys, listeners, patch, reset, autosave, reload ----------------------
void test_changes(const fs::path& base) {
    step("change detection, restart keys, patch / set / reset, listeners, re-entrancy guard");
    const fs::path path = base / L"changes" / L"settings.json";
    SettingsStore store({.path = path});
    std::vector<bs::StoreChange> seen;
    std::vector<std::string> seen_engines;
    const auto id = store.subscribe([&](const Settings& v, const bs::StoreChange& c) {
        seen.push_back(c);
        seen_engines.push_back(v.search_engine);
    });

    auto c1 = store.update([](Settings& s) {
        s.search_engine = "bing";
        s.hardware_acceleration = false;
    });
    STORE_CHECK(c1 && c1->changed && c1->restart_required);
    STORE_CHECK(c1 && same_keys(c1->changed_keys, {"hardware_acceleration", "search_engine"}));
    STORE_CHECK(c1 && same_keys(c1->restart_keys, {"hardware_acceleration"}));
    STORE_CHECK(c1 && c1->source == "native");
    STORE_CHECK(same_keys(store.pending_restart_keys(), {"hardware_acceleration"}));

    const auto write_time = fs::last_write_time(path);
    auto same = store.update([](Settings& s) { s.search_engine = "bing"; });
    STORE_CHECK(same && !same->changed && same->revision == 1 && !same->saved);
    STORE_CHECK(fs::last_write_time(path) == write_time); // unchanged value: nothing written
    auto cancelled = store.update([](Settings& s) {
        s.search_engine = "never";
        return false;
    });
    STORE_CHECK(cancelled && !cancelled->changed && store.get().search_engine == "bing");
    // A change normalize() undoes is no change.
    auto normalized_away = store.update([](Settings& s) { s.screenshare_mode = "invalid"; });
    STORE_CHECK(normalized_away && !normalized_away->changed);

    auto p1 = store.patch_json(R"({"nested":{"level":7},"browser_args":["--a","--b"]})", {.source = "test", .sender_webview = 9, .validate = {}});
    STORE_CHECK(p1 && p1->changed && p1->restart_required && p1->source == "test" && p1->sender_webview == 9);
    STORE_CHECK(p1 && same_keys(p1->changed_keys, {"browser_args", "nested"}));
    STORE_CHECK(store.get().nested.level == 7 && store.get().nested.label == "x"); // nested merge
    STORE_CHECK(store.get().search_engine == "bing");                             // untouched key kept
    STORE_CHECK(store.patch_json(R"({"browser_args":["--c"]})") && store.get().browser_args.size() == 1); // arrays replace
    const auto unknown = store.patch_json(R"({"nope":1})");
    STORE_CHECK(!unknown && unknown.error().code == ERROR_INVALID_DATA);
    STORE_CHECK(!store.patch_json("[1]") && !store.patch_json("null") && !store.patch_json(R"({"counter":"x"})"));
    STORE_CHECK(store.get().nested.level == 7); // failed patches change nothing

    auto vetoed = store.update([](Settings& s) { s.counter = 77; },
        {.source = {}, .sender_webview = 0, .validate = [](const std::vector<std::string>& keys) -> std::expected<void, bs::StoreError> {
            if (std::ranges::find(keys, std::string{"counter"}) != keys.end()) {
                return std::unexpected(bs::StoreError{.code = ERROR_ACCESS_DENIED, .message = "no"});
            }
            return {};
        }});
    STORE_CHECK(!vetoed && vetoed.error().code == ERROR_ACCESS_DENIED && store.get().counter == 0);

    auto reentrant_code = std::uint32_t{0};
    STORE_CHECK(store.update([&](Settings& s) {
        s.counter = 1;
        auto inner = store.update([](Settings& v) { v.counter = 2; });
        reentrant_code = inner ? 0 : inner.error().code;
    }));
    STORE_CHECK(reentrant_code == 1131 && store.get().counter == 1);

    std::uint32_t listener_code = 0;
    const auto id2 = store.subscribe([&](const Settings&, const bs::StoreChange&) {
        auto inner = store.reset();
        listener_code = inner ? 0 : inner.error().code;
        static_cast<void>(store.get()); // reading from a listener is fine
        throw std::runtime_error("listeners may throw");
    });
    STORE_CHECK(store.update([](Settings& s) { s.counter = 3; }).has_value());
    STORE_CHECK(listener_code == 1131);
    STORE_CHECK(store.unsubscribe(id2) && !store.unsubscribe(id2));

    auto r = store.reset();
    STORE_CHECK(r && r->changed && r->source == "reset" && store.get().search_engine == "duckduckgo");
    STORE_CHECK(store.get().browser_args.empty());

    // Listener saw every committed change, in order, with increasing revisions.
    bool increasing = !seen.empty();
    for (std::size_t i = 1; i < seen.size(); ++i) {
        increasing = increasing && seen[i].revision == seen[i - 1].revision + 1;
    }
    STORE_CHECK(increasing);
    STORE_CHECK(!seen_engines.empty() && seen_engines.front() == "bing" && seen_engines.back() == "duckduckgo");
    STORE_CHECK(seen.size() == store.revision());
    STORE_CHECK(store.unsubscribe(id));

    step("set() replaces the whole value; json() / read()");
    Settings full;
    full.search_engine = "startpage";
    full.counter = 10;
    STORE_CHECK(store.set(full) && store.get().counter == 10);
    STORE_CHECK(contains(store.json(), "\"search_engine\":\"startpage\""));
    STORE_CHECK(store.read([](const Settings& s) { return s.counter; }) == 10);

    step("autosave off: dirty until save(); reload() picks up external edits");
    const fs::path manual = base / L"manual" / L"settings.json";
    SettingsStore lazy({.path = manual, .autosave = false});
    auto lc = lazy.update([](Settings& s) { s.counter = 42; });
    STORE_CHECK(lc && lc->changed && !lc->saved && lazy.dirty() && !fs::exists(manual));
    STORE_CHECK(lazy.save().has_value() && !lazy.dirty() && contains(read_all(manual), "\"counter\": 42"));
    write_all(manual, R"({"counter": 43, "search_engine": "edited"})");
    std::vector<std::string> reload_sources;
    lazy.subscribe([&](const Settings&, const bs::StoreChange& c) { reload_sources.push_back(c.source); });
    auto rl = lazy.reload();
    STORE_CHECK(rl && rl->changed && lazy.get().counter == 43 && lazy.get().search_engine == "edited");
    STORE_CHECK(reload_sources.size() == 1 && reload_sources.front() == "reload" && !lazy.dirty());
    STORE_CHECK(rl && same_keys(rl->changed_keys, {"counter", "search_engine"}));
}

// ---- concurrency --------------------------------------------------------------------------------------
void test_concurrency(const fs::path& base) {
    step("concurrent updates from 8 threads + readers: no lost update, ordered notifications");
    const fs::path path = base / L"concurrent" / L"settings.json";
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    {
        SettingsStore store({.path = path, .write = {.flush = false, .keep_backup = true, .before_replace = {}}});
        std::mutex mutex;
        std::vector<std::uint64_t> revisions;
        std::vector<std::uint64_t> counters;
        store.subscribe([&](const Settings& v, const bs::StoreChange& c) {
            std::scoped_lock lock(mutex);
            revisions.push_back(c.revision);
            counters.push_back(v.counter);
        });
        std::atomic<bool> stop{false};
        std::atomic<int> failures{0};
        std::atomic<std::uint64_t> reads{0};
        std::vector<std::jthread> readers;
        for (int r = 0; r < 2; ++r) {
            readers.emplace_back([&]() {
                std::uint64_t last = 0;
                while (!stop.load()) {
                    const Settings s = store.get();
                    if (s.counter < last) {
                        failures.fetch_add(1); // a reader must never see the value go back
                    }
                    last = s.counter;
                    reads.fetch_add(1);
                }
            });
        }
        {
            std::vector<std::jthread> writers;
            for (int t = 0; t < kThreads; ++t) {
                writers.emplace_back([&store, &failures, t]() {
                    for (int i = 0; i < kPerThread; ++i) {
                        auto change = store.update([t](Settings& s) {
                            ++s.counter;
                            s.search_engine = "thread-" + std::to_string(t);
                        });
                        if (!change || !change->changed || !change->saved) {
                            failures.fetch_add(1);
                        }
                    }
                });
            }
        }
        stop = true;
        readers.clear();
        STORE_CHECK(failures.load() == 0);
        STORE_CHECK(reads.load() > 0);
        STORE_CHECK(store.get().counter == kThreads * kPerThread);
        STORE_CHECK(store.revision() == kThreads * kPerThread);
        std::scoped_lock lock(mutex);
        STORE_CHECK(revisions.size() == static_cast<std::size_t>(kThreads * kPerThread));
        bool ordered = true;
        for (std::size_t i = 0; i < revisions.size(); ++i) {
            ordered = ordered && revisions[i] == i + 1 && counters[i] == i + 1;
        }
        STORE_CHECK(ordered);
    }
    SettingsStore reopened({.path = path});
    STORE_CHECK(reopened.load_report().status == bs::LoadStatus::Loaded);
    STORE_CHECK(reopened.get().counter == kThreads * kPerThread);
    STORE_CHECK(files_with(path.parent_path(), L".tmp").empty());
}

// ---- crash during save --------------------------------------------------------------------------------
int run_crash_child(const fs::path& path) {
    bs::StoreOptions options{.path = path};
    options.write.before_replace = [](const fs::path&) {
        // Simulates a crash / power cut between writing the temp file and swapping it in.
        TerminateProcess(GetCurrentProcess(), kCrashExitCode);
    };
    SettingsStore store(std::move(options));
    static_cast<void>(store.update([](Settings& s) { s.search_engine = "written-by-crashed-process"; }));
    return 1; // not reached
}

void test_crash_during_save(const fs::path& base) {
    step("crash during save: temp file left behind, original intact, temp removed on next open");
    const fs::path path = base / L"crash" / L"settings.json";
    {
        SettingsStore store({.path = path});
        STORE_CHECK(store.update([](Settings& s) { s.search_engine = "before-crash"; }).has_value());
    }
    const std::string original = read_all(path);

    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    std::wstring cmd = L"\"" + exe + L"\" --crash-save \"" + path.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    STORE_CHECK(started);
    if (!started) {
        return;
    }
    STORE_CHECK(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    STORE_CHECK(exit_code == kCrashExitCode);

    const auto temps = files_with(path.parent_path(), L".tmp");
    STORE_CHECK(temps.size() == 1);
    STORE_CHECK(!temps.empty() && contains(read_all(temps.front()), "written-by-crashed-process"));
    STORE_CHECK(read_all(path) == original); // byte-for-byte intact

    SettingsStore after({.path = path});
    STORE_CHECK(after.load_report().status == bs::LoadStatus::Loaded);
    STORE_CHECK(after.load_report().stale_temp_files_removed == 1);
    STORE_CHECK(after.get().search_engine == "before-crash");
    STORE_CHECK(files_with(path.parent_path(), L".tmp").empty());
}

// ---- write_file_atomic / default_store_path / senders -------------------------------------------------
void test_atomic_and_paths(const fs::path& base) {
    step("write_file_atomic: create, replace with .bak, no leftovers; default_store_path");
    const fs::path path = base / L"atomic" / L"x" / L"data.json";
    const auto bytes = [](std::string_view s) { return std::as_bytes(std::span<const char>(s.data(), s.size())); };
    STORE_CHECK(bs::write_file_atomic(path, bytes("one")).has_value());
    STORE_CHECK(read_all(path) == "one");
    STORE_CHECK(bs::write_file_atomic(path, bytes("two")).has_value());
    STORE_CHECK(bs::write_file_atomic(path, bytes("three")).has_value());
    STORE_CHECK(read_all(path) == "three" && read_all(bs::detail::backup_path_of(path)) == "two");
    const fs::path nobak = base / L"atomic" / L"nobak.json";
    STORE_CHECK(bs::write_file_atomic(nobak, bytes("a"), {.flush = true, .keep_backup = false, .before_replace = {}}).has_value());
    STORE_CHECK(bs::write_file_atomic(nobak, bytes("b"), {.flush = true, .keep_backup = false, .before_replace = {}}).has_value());
    STORE_CHECK(read_all(nobak) == "b" && !fs::exists(bs::detail::backup_path_of(nobak)));
    STORE_CHECK(files_with(path.parent_path(), L".tmp").empty());
    STORE_CHECK(!bs::write_file_atomic({}, bytes("x")).has_value());

    const auto p = bs::default_store_path(L"Boson Store Test", L"prefs.json");
    STORE_CHECK(p.has_value() && p->filename() == L"prefs.json" && p->parent_path().filename() == L"Boson Store Test");
    STORE_CHECK(!bs::default_store_path(L"..").has_value());
    STORE_CHECK(!bs::default_store_path(L"a\\b").has_value());
    STORE_CHECK(!bs::default_store_path(L"app", L"../x.json").has_value());
    STORE_CHECK(bs::to_string(bs::LoadStatus::RecoveredFromBackup) == "recovered_from_backup");

    step("std::execution: update_on / save_on run on the scheduler");
    boson::exec::Scheduler scheduler(2);
    SettingsStore store({.path = base / L"sender" / L"settings.json", .autosave = false});
    const DWORD caller = GetCurrentThreadId();
    std::atomic<DWORD> worker{0};
    auto result = stdexec::sync_wait(store.update_on(scheduler.get_scheduler(), [&worker](Settings& s) {
        worker = GetCurrentThreadId();
        s.counter = 99;
    }));
    STORE_CHECK(result.has_value());
    if (result) {
        const auto& change = std::get<0>(*result);
        STORE_CHECK(change && change->changed && change->revision == 1);
    }
    STORE_CHECK(worker.load() != 0 && worker.load() != caller);
    auto saved = stdexec::sync_wait(store.save_on(scheduler.get_scheduler()));
    STORE_CHECK(saved.has_value() && std::get<0>(*saved).has_value() && !store.dirty());
}

// ---- IPC controller -----------------------------------------------------------------------------------
using SettingsController = boson::ipc::StoreController<Settings>;
using PrefsController = boson::ipc::StoreController<Settings, "Prefs">;

void test_controller(const fs::path& base) {
    step("StoreController via Router: get / set / patch / reset / info, read-only keys");
    auto store = std::make_shared<SettingsStore>(bs::StoreOptions{.path = base / L"ctl" / L"settings.json", .name = "main"});
    SettingsController controller(store, {.read_only_keys = {"browser_args"}});
    auto prefs_store = std::make_shared<SettingsStore>(bs::StoreOptions{.name = "prefs"});
    PrefsController prefs(prefs_store);
    static_assert(SettingsController::boson_controller_name() == "Store");
    static_assert(PrefsController::boson_controller_name() == "Prefs");
    boson::ipc::Router<SettingsController, PrefsController> router(controller, prefs);

    std::vector<bs::StoreChange> changes;
    store->subscribe([&](const Settings&, const bs::StoreChange& c) { changes.push_back(c); });

    const std::string got = router.dispatch_sync(R"({"id":1,"method":"Store.get","params":null})");
    STORE_CHECK(contains(got, "\"status\":\"success\"") && contains(got, "\"search_engine\":\"duckduckgo\""));

    std::string patched;
    {
        const boson::ipc::SenderScope sender(42);
        patched = router.dispatch_sync(R"({"id":2,"method":"Store.patch","params":{"search_engine":"bing","hardware_acceleration":false}})");
    }
    STORE_CHECK(contains(patched, "\"status\":\"success\"") && contains(patched, "\"restart_required\":true"));
    STORE_CHECK(contains(patched, "\"restart_keys\":[\"hardware_acceleration\"]"));
    STORE_CHECK(store->get().search_engine == "bing" && !store->get().hardware_acceleration);
    STORE_CHECK(!changes.empty() && changes.back().source == "ipc" && changes.back().sender_webview == 42);

    const std::string ro = router.dispatch_sync(R"({"id":3,"method":"Store.patch","params":{"browser_args":["--evil"]}})");
    STORE_CHECK(contains(ro, "\"status\":\"error\"") && contains(ro, "read-only"));
    STORE_CHECK(store->get().browser_args.empty());
    const std::string bad = router.dispatch_sync(R"({"id":4,"method":"Store.patch","params":{"unknown_key":1}})");
    STORE_CHECK(contains(bad, "\"status\":\"error\""));
    const std::string not_obj = router.dispatch_sync(R"({"id":5,"method":"Store.patch","params":"x"})");
    STORE_CHECK(contains(not_obj, "\"status\":\"error\""));

    // C++ may still change read-only keys; set() from IPC keeps them only if unchanged.
    STORE_CHECK(store->update([](Settings& s) { s.browser_args = {"--native"}; }).has_value());
    const std::string set_ok = router.dispatch_sync(
        R"({"id":6,"method":"Store.set","params":{"search_engine":"kagi","browser_args":["--native"],"tab_sleep_minutes":5000}})");
    STORE_CHECK(contains(set_ok, "\"status\":\"success\""));
    STORE_CHECK(store->get().search_engine == "kagi" && store->get().tab_sleep_minutes == 1440); // normalized
    STORE_CHECK(store->get().hardware_acceleration);                                            // set = full replace
    const std::string set_ro = router.dispatch_sync(R"({"id":7,"method":"Store.set","params":{"search_engine":"x"}})");
    STORE_CHECK(contains(set_ro, "read-only") && store->get().search_engine == "kagi");

    const std::string reset = router.dispatch_sync(R"({"id":8,"method":"Store.reset"})");
    STORE_CHECK(contains(reset, "\"status\":\"success\"") && contains(reset, "\"source\":\"reset\""));
    STORE_CHECK(store->get().search_engine == "duckduckgo" && store->get().browser_args.size() == 1); // read-only kept

    const std::string info = router.dispatch_sync(R"({"id":9,"method":"Store.info"})");
    STORE_CHECK(contains(info, "\"name\":\"main\"") && contains(info, "\"load_status\":\"missing\""));
    STORE_CHECK(contains(info, "\"pending_restart_keys\":[\"hardware_acceleration\",\"browser_args\"]"));
    STORE_CHECK(contains(info, "\"read_only_keys\":[\"browser_args\"]") && contains(info, "settings.json"));

    const std::string other = router.dispatch_sync(R"({"id":10,"method":"Prefs.patch","params":{"counter":3}})");
    STORE_CHECK(contains(other, "\"status\":\"success\"") && prefs_store->get().counter == 3 && store->get().counter == 0);

    step("connect_store_events: every committed change is posted as store:changed");
    boson::exec::UiMarshal marshal([]() {});
    boson::exec::Scheduler scheduler(1);
    boson::exec::TaskRunner runner(scheduler, marshal);
    boson::ipc::Bridge bridge(runner, marshal, router);
    auto events = connect_store_events(controller, bridge);
    STORE_CHECK(events.active());
    const std::size_t before = marshal.pending_count();
    STORE_CHECK(store->update([](Settings& s) { s.counter = 1; }).has_value());
    STORE_CHECK(router.dispatch_sync(R"({"id":11,"method":"Store.patch","params":{"counter":2}})").find("success") != std::string::npos);
    STORE_CHECK(marshal.pending_count() == before + 2);
    events.reset();
    STORE_CHECK(!events.active());
    STORE_CHECK(store->update([](Settings& s) { s.counter = 3; }).has_value());
    STORE_CHECK(marshal.pending_count() == before + 2);
    marshal.drain_on_ui_thread();
    STORE_CHECK(&store_of(controller) == store.get());

    step("TypeScript bindings for Store<T>");
    const std::string dts = boson::ipc::TsGenerator::generate_with_events<boson::ipc::StoreEvents<Settings>, SettingsController, PrefsController>();
    STORE_CHECK(contains(dts, "export interface TestSettings {"));
    STORE_CHECK(contains(dts, "\"Store.get\": () => Promise<TestSettings>;"));
    STORE_CHECK(contains(dts, "\"Store.patch\": (params: Partial<TestSettings>) => Promise<StoreChange>;"));
    STORE_CHECK(contains(dts, "\"Store.set\": (params: TestSettings) => Promise<StoreChange>;"));
    STORE_CHECK(contains(dts, "\"Prefs.info\": () => Promise<StoreInfo>;"));
    STORE_CHECK(contains(dts, "\"store:changed\": StoreChangedEvent_TestSettings;"));
    STORE_CHECK(contains(dts, "export interface StoreChangedEvent_TestSettings {"));
    STORE_CHECK(contains(dts, "restart_required: boolean;"));
    STORE_CHECK(boson::ipc::detail::ts_identifier("struct boson::ipc::StoreChangedEvent<struct stt::TestSettings>") == "StoreChangedEvent_TestSettings");
    STORE_CHECK(boson::ipc::detail::ts_identifier("boson::ipc::FsStat") == "FsStat");
    std::cout << "  " << dts.substr(dts.find("export interface BosonApi"), 200) << "...\n";
}

// ---- WebView end to end (skips without a runtime) -----------------------------------------------------
bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js) {
    bool done = false;
    std::optional<std::string> result;
    if (!webview.execute_script(js, [&](HRESULT hr, std::string value) {
            if (SUCCEEDED(hr)) {
                result = std::move(value);
            }
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return result;
}

const std::string kPage = "<!doctype html><html><head><title>store</title></head><body>store</body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::as_bytes(std::span<const char>(kPage.data(), kPage.size()));
    }
    return std::nullopt;
}

void test_webview(const fs::path& base) {
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
        std::cout << "SKIP: no WebView2 runtime installed (end-to-end Store checks not run)\n";
        return;
    }
    CoTaskMemFree(version);
    step("WebView: Store.* through Bridge + CapabilityPolicy, store:changed reaches the page");

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson store test";
    window_cfg.width = 480;
    window_cfg.height = 320;
    window_cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    STORE_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = (base / L"udf").wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
    host->set_default_asset_lookup(&lookup);
    boson::core::WebViewConfig cfg;
    cfg.initial_url = "boson://app/index.html";
    bool nav_done = false;
    boson::core::WebView& wv = host->create(cfg);
    wv.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { nav_done = e.is_success; };
    STORE_CHECK(pump_until([&]() { return wv.is_ready() && nav_done; }));

    auto store = std::make_shared<SettingsStore>(bs::StoreOptions{.path = base / L"wv" / L"settings.json"});
    {
        SettingsController controller(store);
        boson::ipc::Router<SettingsController> router(controller);
        boson::ipc::Bridge bridge(runner, marshal, router);
        namespace sec = boson::security;
        sec::CapabilityConfig caps;
        caps.capabilities.push_back(sec::Capability{
            .identifier = "settings-ui", .description = {}, .origins = {"boson://app"},
            .allow = {"Store.get", "Store.patch", "Store.info"}, .deny = {"Store.reset"}});
        auto policy = sec::CapabilityPolicy::make_shared(caps);
        STORE_CHECK(policy.has_value());
        bridge.attach(wv, {.allowed_origins = {}, .capabilities = policy.value_or(nullptr)});
        const auto events = connect_store_events(controller, bridge);

        STORE_CHECK(eval(wv, "window.__r = []; window.__ev = []; chrome.webview.addEventListener('message', e => {"
                             " if (e.data && e.data.type === 'event') window.__ev.push(e.data); else window.__r.push(e.data); }); 1"));
        STORE_CHECK(eval(wv, "chrome.webview.postMessage({id: 1, method: 'Store.patch', params: {tab_sleep_minutes: 30}});"
                             "chrome.webview.postMessage({id: 2, method: 'Store.set', params: {}});"
                             "chrome.webview.postMessage({id: 3, method: 'Store.reset', params: null}); 1"));
        STORE_CHECK(pump_until([&]() { return eval(wv, "window.__r.length") == std::optional<std::string>("3"); }, std::chrono::seconds{10}));
        const auto r1 = eval(wv, "JSON.stringify(window.__r.find(x => x.id === 1))");
        const auto r2 = eval(wv, "JSON.stringify(window.__r.find(x => x.id === 2))");
        const auto r3 = eval(wv, "JSON.stringify(window.__r.find(x => x.id === 3))");
        STORE_CHECK(contains(r1, "success") && contains(r1, "tab_sleep_minutes"));
        STORE_CHECK(contains(r2, "-32003"));
        STORE_CHECK(contains(r3, "-32003"));
        STORE_CHECK(store->get().tab_sleep_minutes == 30);

        // A change made in C++ reaches the page too.
        STORE_CHECK(store->update([](Settings& s) { s.search_engine = "from-cpp"; }).has_value());
        STORE_CHECK(pump_until([&]() { return eval(wv, "window.__ev.length") == std::optional<std::string>("2"); }, std::chrono::seconds{10}));
        const auto ev = eval(wv, "JSON.stringify(window.__ev.map(e => [e.event, e.data.store, e.data.value.search_engine, e.data.change.source]))");
        std::cout << "  events: " << ev.value_or("<none>") << '\n';
        STORE_CHECK(contains(ev, R"([\"store:changed\",\"settings\",\"duckduckgo\",\"ipc\"])"));
        STORE_CHECK(contains(ev, R"([\"store:changed\",\"settings\",\"from-cpp\",\"native\"])"));
        bridge.detach(wv);
    }
    bool shut = false;
    env->shutdown([&]() { shut = true; });
    pump_until([&]() { return shut; }, std::chrono::seconds{10});
    host.reset();
    window.reset();
}

std::optional<fs::path> crash_child_arg() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::optional<fs::path> out;
    if (argv != nullptr) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::wstring_view{argv[i]} == L"--crash-save") {
                out = fs::path{argv[i + 1]};
            }
        }
        LocalFree(argv);
    }
    return out;
}

} // namespace

int main() {
    if (const auto child = crash_child_arg()) {
        return run_crash_child(*child);
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    const fs::path base = fs::temp_directory_path() /
        (L"boson-store-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(base);

    test_missing(base);
    test_corrupt(base);
    test_schema_and_normalize(base);
    test_changes(base);
    test_concurrency(base);
    test_crash_during_save(base);
    test_atomic_and_paths(base);
    test_controller(base);
    test_webview(base);

    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        fs::remove_all(base, ec);
        if (!fs::exists(base)) {
            break;
        }
        Sleep(250); // WebView2 may still hold the user data folder briefly
    }
    STORE_CHECK(!fs::exists(base));

    std::cout << (g_checks - g_failures) << "/" << g_checks << " store checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
