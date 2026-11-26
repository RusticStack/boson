#include <boson/shipping/webview2_runtime.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/shipping/authenticode.hpp>

#include "file_util.hpp"

#include <WebView2.h>
#include <shellapi.h>

namespace boson::shipping {

namespace {
constexpr std::string_view kCategory = "webview2";

/// Writes bytes to path (create-new semantics are not needed: the name carries the process id).
bool write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) noexcept {
    const detail::UniqueHandle file = detail::make_handle(
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + done, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}
}

std::optional<std::wstring> installed_webview2_version(std::wstring_view browser_folder) {
    const std::wstring folder(browser_folder);
    LPWSTR version = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(folder.empty() ? nullptr : folder.c_str(), &version);
    if (FAILED(hr) || version == nullptr) {
        if (version != nullptr) {
            CoTaskMemFree(version);
        }
        return std::nullopt;
    }
    std::wstring out(version);
    CoTaskMemFree(version);
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

std::string_view to_string(RuntimeStatus status) noexcept {
    switch (status) {
    case RuntimeStatus::AlreadyInstalled: return "already_installed";
    case RuntimeStatus::Installed: return "installed";
    case RuntimeStatus::ConsentDeclined: return "consent_declined";
    case RuntimeStatus::DownloadFailed: return "download_failed";
    case RuntimeStatus::VerificationFailed: return "verification_failed";
    case RuntimeStatus::InstallerFailed: return "installer_failed";
    case RuntimeStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

RuntimeEnsureResult ensure_webview2_runtime(const RuntimeBootstrapOptions& options, std::stop_token stop) {
    RuntimeEnsureResult result;
    const auto log = [&](LogLevel level, std::string_view message) { log_message(options.logger, level, kCategory, message); };

    if (!options.assume_missing) {
        if (auto version = installed_webview2_version(options.browser_folder)) {
            result.version = std::move(*version);
            return result;
        }
    }
    log(LogLevel::Warn, "WebView2 runtime not found");

    if (options.consent) {
        bool agreed = false;
        try {
            agreed = options.consent();
        } catch (...) {
            agreed = false;
        }
        if (!agreed) {
            result.status = RuntimeStatus::ConsentDeclined;
            result.error = "the user declined the WebView2 runtime install";
            log(LogLevel::Info, result.error);
            return result;
        }
    }
    if (stop.stop_requested()) {
        result.status = RuntimeStatus::Cancelled;
        return result;
    }

    const std::filesystem::path dir = options.download_dir.empty() ? detail::temp_directory() : options.download_dir;
    const auto setup = dir / (L"MicrosoftEdgeWebview2Setup-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            DeleteFileW(path.c_str());
            std::filesystem::path partial = path;
            partial += L".partial";
            DeleteFileW(partial.c_str());
            partial += L".json";
            DeleteFileW(partial.c_str());
        }
    } cleanup{setup};
    if (!options.embedded_bootstrapper.empty()) {
        if (!write_bytes(setup, options.embedded_bootstrapper)) {
            result.status = RuntimeStatus::DownloadFailed;
            result.error = "cannot write the embedded bootstrapper (" + detail::win32_message(GetLastError()) + ")";
            log(LogLevel::Error, result.error);
            return result;
        }
    } else {
        log(LogLevel::Info, "downloading the WebView2 bootstrapper from " + options.bootstrapper_url);
        const DownloadResult downloaded = download_file(
            options.bootstrapper_url, setup, options.http, options.on_progress, stop, DownloadOptions{.resume = false});
        if (!downloaded.ok) {
            result.status = downloaded.cancelled ? RuntimeStatus::Cancelled : RuntimeStatus::DownloadFailed;
            result.error = downloaded.error;
            log(LogLevel::Error, "bootstrapper download failed: " + downloaded.error);
            return result;
        }
    }

    const AuthenticodeResult signature = verify_authenticode(setup, options.required_signer);
    if (!signature.ok()) {
        result.status = RuntimeStatus::VerificationFailed;
        result.error = "bootstrapper signature rejected: " + signature.error;
        log(LogLevel::Error, result.error);
        return result;
    }

    std::wstring command_line = app::quote_argument(setup.wstring());
    if (!options.installer_args.empty()) {
        command_line += L" " + options.installer_args;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        result.status = RuntimeStatus::InstallerFailed;
        result.error = "cannot start the bootstrapper (" + detail::win32_message(GetLastError()) + ")";
        log(LogLevel::Error, result.error);
        return result;
    }
    const detail::UniqueHandle process(pi.hProcess);
    CloseHandle(pi.hThread);
    const auto deadline = std::chrono::steady_clock::now() + options.install_timeout;
    while (WaitForSingleObject(process.get(), 200) == WAIT_TIMEOUT) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            // The bootstrapper hands off to the Edge updater; it is left to finish on its own.
            result.status = stop.stop_requested() ? RuntimeStatus::Cancelled : RuntimeStatus::InstallerFailed;
            result.error = stop.stop_requested() ? "cancelled while installing" : "the bootstrapper timed out";
            log(LogLevel::Error, result.error);
            return result;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    result.installer_exit_code = code;
    if (auto version = installed_webview2_version(options.browser_folder)) {
        result.status = RuntimeStatus::Installed;
        result.version = std::move(*version);
        log(LogLevel::Info, "WebView2 runtime " + core::wide_to_utf8(result.version) + " installed");
        return result;
    }
    result.status = RuntimeStatus::InstallerFailed;
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "the bootstrapper exited with 0x%08lX and no runtime is available", static_cast<unsigned long>(code));
    result.error = buffer;
    log(LogLevel::Error, result.error);
    return result;
}

void ensure_webview2_runtime_async(
    exec::TaskRunner& runner,
    exec::UiMarshal& marshal,
    RuntimeBootstrapOptions options,
    std::function<void(RuntimeEnsureResult)> on_done) {
    if (options.consent) {
        // The prompt belongs on the UI thread; the pool thread waits for the answer.
        options.consent = [&marshal, ask = std::move(options.consent)]() { return marshal.invoke_sync([&ask]() { return ask(); }); };
    }
    runner.spawn([&marshal, options = std::move(options), done = std::move(on_done)]() mutable {
        RuntimeEnsureResult result = ensure_webview2_runtime(options);
        if (done) {
            marshal.post_task([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
        }
    });
}

bool require_webview2_runtime(std::wstring_view app_name, const RuntimeGuardOptions& options) {
    const std::wstring name(app_name);
    RuntimeBootstrapOptions bootstrap;
    bootstrap.embedded_bootstrapper = options.embedded_bootstrapper;
    bootstrap.browser_folder = options.browser_folder;
    bootstrap.logger = options.logger;
    bootstrap.assume_missing = options.assume_missing;
    if (!options.silent) {
        bootstrap.consent = message_box_consent(options.owner, name);
    }
    if (options.embedded_bootstrapper.empty()) {
        // Single-exe default: nothing is downloaded behind the user's back.
        if (!options.assume_missing && installed_webview2_version(options.browser_folder)) {
            return true;
        }
        log_message(options.logger, LogLevel::Error, kCategory, "WebView2 runtime not found and no bootstrapper is embedded");
        if (!options.silent) {
            const std::wstring text = name +
                                      L" needs the Microsoft Edge WebView2 Runtime, which is not installed on this computer.\n\n"
                                      L"Download and install it (free, from Microsoft):\n" +
                                      core::utf8_to_wide(kWebView2DownloadPage) + L"\n\nOpen the download page now?";
            if (MessageBoxW(options.owner, text.c_str(), name.c_str(), MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND) == IDYES) {
                ShellExecuteW(nullptr, L"open", core::utf8_to_wide(kWebView2DownloadPage).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        return false;
    }
    const RuntimeEnsureResult result = ensure_webview2_runtime(bootstrap);
    if (result.ok()) {
        return true;
    }
    if (!options.silent && result.status != RuntimeStatus::ConsentDeclined) {
        const std::wstring text = name + L" could not install the Microsoft Edge WebView2 Runtime:\n\n" +
                                  core::utf8_to_wide(result.error) + L"\n\nInstall it manually from:\n" +
                                  core::utf8_to_wide(kWebView2DownloadPage);
        MessageBoxW(options.owner, text.c_str(), name.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    return false;
}

std::function<bool()> message_box_consent(HWND owner, std::wstring app_name) {
    return [owner, name = std::move(app_name)]() {
        const std::wstring text = name +
                                  L" needs the Microsoft Edge WebView2 Runtime, which is not installed on this computer.\n\n"
                                  L"Download it from Microsoft (about 2 MB, then the runtime itself) and install it now?";
        return MessageBoxW(owner, text.c_str(), name.c_str(), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    };
}

} // namespace boson::shipping
