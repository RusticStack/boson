#pragma once

#include <windows.h>

#include <boson/shipping/http.hpp>
#include <boson/shipping/logger.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace boson::exec {
class TaskRunner;
class UiMarshal;
} // namespace boson::exec

namespace boson::shipping {

/// Microsoft's Evergreen WebView2 bootstrapper (MicrosoftEdgeWebview2Setup.exe, ~2 MB).
inline constexpr std::string_view kWebView2BootstrapperUrl = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";

/// @brief Version of the WebView2 runtime the loader would use (GetAvailableCoreWebView2BrowserVersionString),
/// or std::nullopt when none is installed. browser_folder is a fixed-version runtime folder (empty = evergreen).
[[nodiscard]] std::optional<std::wstring> installed_webview2_version(std::wstring_view browser_folder = {});

enum class RuntimeStatus {
    AlreadyInstalled,
    Installed,
    ConsentDeclined,
    DownloadFailed,
    VerificationFailed,
    InstallerFailed,
    Cancelled,
};

[[nodiscard]] std::string_view to_string(RuntimeStatus status) noexcept;

struct RuntimeEnsureResult {
    RuntimeStatus status = RuntimeStatus::AlreadyInstalled;
    std::wstring version;
    std::string error;
    std::uint32_t installer_exit_code = 0;

    [[nodiscard]] bool ok() const noexcept {
        return status == RuntimeStatus::AlreadyInstalled || status == RuntimeStatus::Installed;
    }
};

struct RuntimeBootstrapOptions {
    std::string bootstrapper_url = std::string(kWebView2BootstrapperUrl);
    /// Asked before anything is downloaded; return false to abort. Empty = consent assumed (only do that
    /// when your installer / EULA already covers it). Runs on the thread that calls ensure_webview2_runtime().
    std::function<bool()> consent;
    DownloadProgressFn on_progress;
    std::filesystem::path download_dir; ///< Default: %TEMP%.
    /// Bootstrapper switches. "/silent /install" installs without UI (per-machine when elevated,
    /// per-user otherwise).
    std::wstring installer_args = L"/silent /install";
    std::chrono::milliseconds install_timeout{std::chrono::minutes{10}};
    /// The bootstrapper must carry a valid Authenticode signature from this signer.
    std::wstring required_signer = L"Microsoft Corporation";
    std::wstring browser_folder; ///< Fixed-version runtime folder to check instead of the evergreen one.
    HttpOptions http;
    std::shared_ptr<Logger> logger;
    /// Testing aid: behave as if no runtime were installed.
    bool assume_missing = false;
};

/// @brief Checks for the runtime and, when it is missing, asks for consent, downloads the bootstrapper over
/// HTTPS with WinHTTP, verifies its Authenticode signature, runs it silently and checks again.
/// Blocking (the install takes up to minutes): call it before creating windows, or use the async form.
[[nodiscard]] RuntimeEnsureResult ensure_webview2_runtime(const RuntimeBootstrapOptions& options = {}, std::stop_token stop = {});

/// @brief Runs ensure_webview2_runtime on the TaskRunner pool; on_done runs on the UI thread. When the
/// runtime is missing, options.consent is invoked on the UI thread (through UiMarshal::invoke_sync).
void ensure_webview2_runtime_async(
    exec::TaskRunner& runner,
    exec::UiMarshal& marshal,
    RuntimeBootstrapOptions options,
    std::function<void(RuntimeEnsureResult)> on_done);

/// @brief A ready-made consent callback: a Yes/No MessageBox explaining the download.
[[nodiscard]] std::function<bool()> message_box_consent(HWND owner, std::wstring app_name);

} // namespace boson::shipping
