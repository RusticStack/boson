#pragma once

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/core/result.hpp>
#include <boson/ipc/export.hpp>

namespace boson::demo {

struct PingResult {
    bool pong = true;
    std::int64_t timestamp_us = 0;
};

struct SystemInfo {
    std::string os;
    std::string architecture;
    std::uint32_t cpu_cores = 0;
    std::string webview2_runtime;
    std::string build_mode;
};

struct ComputePrimesRequest {
    std::uint32_t limit = 2'000'000;
};

struct ComputePrimesResult {
    std::uint32_t limit = 0;
    std::uint64_t count = 0;
    std::uint32_t largest_prime = 0;
    double elapsed_ms = 0.0;
};

class AppController {
public:
    BOSON_CONTROLLER("App");

    [[nodiscard]] PingResult ping() const {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return PingResult{.pong = true, .timestamp_us = static_cast<std::int64_t>(us)};
    }

    [[nodiscard]] SystemInfo get_system_info() const {
        SYSTEM_INFO sys_info{};
        ::GetNativeSystemInfo(&sys_info);

        std::string arch = "unknown";
        switch (sys_info.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64:
                arch = "x86_64 (AMD64)";
                break;
            case PROCESSOR_ARCHITECTURE_ARM64:
                arch = "ARM64";
                break;
            case PROCESSOR_ARCHITECTURE_INTEL:
                arch = "x86";
                break;
            default:
                break;
        }

        std::string wv2_version = "unknown";
        LPWSTR raw_version = nullptr;
        if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &raw_version)) &&
            raw_version != nullptr) {
            wv2_version = boson::core::wide_to_utf8(raw_version);
            ::CoTaskMemFree(raw_version);
        }

#ifdef NDEBUG
        constexpr const char* kBuildMode = "Release (LTO)";
#else
        constexpr const char* kBuildMode = "Debug";
#endif

        return SystemInfo{
            .os = "Windows (Win32 / COM)",
            .architecture = std::move(arch),
            .cpu_cores = static_cast<std::uint32_t>(sys_info.dwNumberOfProcessors),
            .webview2_runtime = std::move(wv2_version),
            .build_mode = kBuildMode,
        };
    }

    [[nodiscard]] ComputePrimesResult compute_primes(ComputePrimesRequest req) const {
        const std::uint32_t limit = req.limit > 50'000'000u ? 50'000'000u : req.limit;
        const auto t0 = std::chrono::steady_clock::now();

        if (limit < 2) {
            return ComputePrimesResult{
                .limit = limit,
                .count = 0,
                .largest_prime = 0,
                .elapsed_ms = 0.0,
            };
        }

        std::vector<std::uint8_t> is_prime(static_cast<std::size_t>(limit) + 1, 1);
        is_prime[0] = 0;
        is_prime[1] = 0;

        for (std::uint32_t p = 2; static_cast<std::uint64_t>(p) * p <= limit; ++p) {
            if (is_prime[p] != 0) {
                for (std::uint32_t m = p * p; m <= limit; m += p) {
                    is_prime[m] = 0;
                }
            }
        }

        std::uint64_t count = 0;
        std::uint32_t largest = 0;
        for (std::uint32_t i = 2; i <= limit; ++i) {
            if (is_prime[i] != 0) {
                ++count;
                largest = i;
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        return ComputePrimesResult{
            .limit = limit,
            .count = count,
            .largest_prime = largest,
            .elapsed_ms = elapsed_ms,
        };
    }

    void trigger_error() const {
        throw std::runtime_error(
            "Simulated C++ exception from AppController::trigger_error (caught at ABI boundary)");
    }
};

} // namespace boson::demo

template <>
struct glz::meta<boson::demo::AppController> {
    using T = boson::demo::AppController;
    static constexpr auto value = glz::object(
        "ping", &T::ping,
        "get_system_info", &T::get_system_info,
        "compute_primes", &T::compute_primes,
        "trigger_error", &T::trigger_error);
};
