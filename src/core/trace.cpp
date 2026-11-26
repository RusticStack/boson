#include <boson/core/trace.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace boson::trace {

namespace {

constexpr std::size_t kCapacity = 1024;

struct Entry {
    const char* name = nullptr;
    std::int64_t qpc = 0;
    DWORD thread_id = 0;
};

std::array<Entry, kCapacity> g_entries{};
std::atomic<std::size_t> g_count{0};
std::atomic<bool> g_flushed{false};
std::wstring g_target; // empty: debugger + stderr

/// QPC value that corresponds to the moment the process was created, derived once at start-up.
std::int64_t process_start_qpc() noexcept {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    LARGE_INTEGER now_qpc{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&now_qpc);
    QueryPerformanceFrequency(&freq);
    FILETIME now_ft{};
    GetSystemTimePreciseAsFileTime(&now_ft);
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return now_qpc.QuadPart;
    }
    const auto to_u64 = [](const FILETIME& ft) {
        return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const std::uint64_t age_100ns = to_u64(now_ft) - to_u64(created);
    return now_qpc.QuadPart - static_cast<std::int64_t>(age_100ns * static_cast<std::uint64_t>(freq.QuadPart) / 10'000'000ull);
}

std::int64_t g_start_qpc = 0;

void flush_at_exit() noexcept { flush(); }

struct Init {
    Init() noexcept {
        wchar_t value[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"BOSON_TRACE", value, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return;
        }
        const std::wstring v(value, len);
        if (v != L"0") {
            if (v != L"1") {
                g_target = v;
            }
            g_start_qpc = process_start_qpc();
            detail::g_enabled = true;
            std::atexit(&flush_at_exit);
            detail::record("trace.init", [] {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                return now.QuadPart;
            }());
        }
    }
};

const Init g_init;

} // namespace

void detail::record(const char* name, std::int64_t qpc) noexcept {
    const std::size_t slot = g_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < kCapacity) {
        g_entries[slot] = Entry{name, qpc, GetCurrentThreadId()};
    }
}

void flush() noexcept {
    if (!g_flushed.exchange(true)) {
        mark("trace.flush");
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        const std::size_t count = std::min(g_count.load(std::memory_order_relaxed), kCapacity);
        std::string out;
        out.reserve(count * 64);
        char line[160];
        // Entries are written by several threads: order by time.
        std::array<std::size_t, kCapacity> order{};
        for (std::size_t i = 0; i < count; ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count), [](std::size_t a, std::size_t b) {
            return g_entries[a].qpc < g_entries[b].qpc;
        });
        std::int64_t previous = g_start_qpc;
        const DWORD pid = GetCurrentProcessId();
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = g_entries[order[i]];
            const double at_ms = static_cast<double>(e.qpc - g_start_qpc) * 1000.0 / static_cast<double>(freq.QuadPart);
            const double delta_ms = static_cast<double>(e.qpc - previous) * 1000.0 / static_cast<double>(freq.QuadPart);
            previous = e.qpc;
            const int n = std::snprintf(line, sizeof(line), "[boson-trace pid=%lu tid=%lu] %9.3f  +%8.3f  %s\n",
                static_cast<unsigned long>(pid), static_cast<unsigned long>(e.thread_id), at_ms, delta_ms, e.name);
            if (n > 0) {
                out.append(line, static_cast<std::size_t>(n));
            }
        }
        if (g_target.empty()) {
            OutputDebugStringA(out.c_str());
            std::fwrite(out.data(), 1, out.size(), stderr);
        } else {
            const HANDLE file = CreateFileW(g_target.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
                CloseHandle(file);
            }
        }
    }
}

} // namespace boson::trace
