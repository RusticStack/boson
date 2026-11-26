#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace boson::trace {

namespace detail {
/// Set once, before main(), from the BOSON_TRACE environment variable (src/core/trace.cpp).
inline bool g_enabled = false;
void record(const char* name, std::int64_t qpc) noexcept;
} // namespace detail

/// @brief True when BOSON_TRACE is set. One load of a plain bool: this is the whole cost of a disabled marker.
[[nodiscard]] inline bool enabled() noexcept { return detail::g_enabled; }

/// @brief Startup/shutdown timeline marker. `name` must be a string literal (it is stored, not copied).
///
/// When BOSON_TRACE is unset this is a single predictable branch and no call. When set, the marker stores a
/// QueryPerformanceCounter value in a fixed lock-free table (no allocation, safe from any thread). The table is
/// written at process exit as "<ms since process creation> <delta ms> <name>" lines:
///   BOSON_TRACE=1            -> OutputDebugString and stderr
///   BOSON_TRACE=<file path>  -> appended to that file
inline void mark(const char* name) noexcept {
    if (!detail::g_enabled) {
        return;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    detail::record(name, now.QuadPart);
}

/// @brief Writes the table now (also done by an atexit handler). Call it before ExitProcess/TerminateProcess.
void flush() noexcept;

} // namespace boson::trace
