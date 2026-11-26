#pragma once

#include <windows.h>

#include <boson/shipping/http.hpp>
#include <boson/shipping/logger.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace boson::exec {
class TaskRunner;
class UiMarshal;
} // namespace boson::exec

namespace boson::shipping {

inline constexpr std::wstring_view kUpdateHelperSwitch = L"--boson-update-helper=";
inline constexpr std::wstring_view kUpdatedSwitch = L"--boson-updated=";
inline constexpr std::wstring_view kUpdateFailedSwitch = L"--boson-update-failed=";

enum class PackageKind {
    Auto, ///< Decide from the manifest "kind" field or the package URL extension.
    Msi,  ///< Windows Installer package: msiexec /i after the app exits.
    Zip,  ///< Portable archive: files are swapped in the install folder after the app exits, with rollback.
    Exe,  ///< Third-party installer executable run with manifest "installer_args".
};

[[nodiscard]] std::string_view to_string(PackageKind kind) noexcept;
[[nodiscard]] PackageKind parse_package_kind(std::string_view name) noexcept;
/// @brief Kind implied by the URL path's extension (.msi / .zip / .exe), else Auto.
[[nodiscard]] PackageKind package_kind_from_url(std::string_view url) noexcept;

/// @brief One downloadable artifact. Signature = base64 ECDSA P-256 (r || s) over update_signing_payload().
struct UpdatePackage {
    std::string url;
    std::string sha256;
    std::string signature;
    std::optional<std::uint64_t> size;
    std::string installer_args;
};

/// @brief The JSON document served at UpdaterConfig::manifest_url.
///
///     {
///       "version": "1.4.0",
///       "url": "https://example.com/app-1.4.0.zip",     // may be relative to the manifest URL
///       "sha256": "<64 hex>", "signature": "<base64>", "size": 1234567,
///       "notes": "What's new", "mandatory": false, "rollout": 25,
///       "minimum_version": "1.0.0", "pub_date": "2026-11-26T00:00:00Z",
///       "packages": { "msi": { "url": "...", "sha256": "...", "signature": "..." } }
///     }
struct UpdateManifest {
    std::string version;
    std::string url;
    std::string sha256;
    std::string signature;
    std::optional<std::uint64_t> size;
    std::string kind;             ///< "msi" | "zip" | "exe"; empty = from the URL extension.
    std::string installer_args;   ///< Arguments for kind "exe" (e.g. "/S" or "/VERYSILENT").
    std::string notes;
    bool mandatory = false;
    double rollout = 100.0;       ///< Staged rollout percentage, 0..100.
    std::string minimum_version;  ///< Installs older than this treat the update as mandatory.
    std::string pub_date;
    /// Per-install-kind packages ("msi", "zip", "exe"); preferred over the top-level url when the app's
    /// UpdaterConfig::install_kind has an entry here.
    std::map<std::string, UpdatePackage> packages;
};

struct ManifestParseResult {
    std::optional<UpdateManifest> manifest;
    std::string error;
};

/// @brief Parses and validates a manifest (unknown keys are ignored). Relative package URLs are resolved
/// against manifest_url.
[[nodiscard]] ManifestParseResult parse_update_manifest(std::string_view json, std::string_view manifest_url = {});

/// @brief The exact bytes that are signed for one package: "boson-update-v1\n<version>\n<sha256 lower hex>\n".
/// Binding the version stops a valid old package from being replayed as a newer one.
[[nodiscard]] std::string update_signing_payload(std::string_view version, std::string_view sha256_hex);

/// @brief Deterministic bucket 0..9999 for (install_id, version).
[[nodiscard]] std::uint32_t rollout_bucket(std::string_view install_id, std::string_view version);
[[nodiscard]] bool in_rollout(double percentage, std::string_view install_id, std::string_view version);
/// @brief Stable anonymous id: SHA-256 of the machine GUID and user SID (nothing is written anywhere).
[[nodiscard]] std::string default_install_id();

struct UpdaterConfig {
    std::string manifest_url;
    std::string current_version;
    /// base64 of the 64-byte ECDSA P-256 public key (X || Y), as printed by boson_updater_sign keygen.
    std::string public_key;
    /// Folder name under %LOCALAPPDATA% when work_dir is empty.
    std::wstring app_id = L"BosonApp";
    /// Downloads, staging, helper and result files. Default: %LOCALAPPDATA%\<app_id>\updates.
    std::filesystem::path work_dir;
    std::string install_id;                     ///< Empty: default_install_id().
    PackageKind install_kind = PackageKind::Auto;
    /// Portable (Zip) installs: the folder whose files are replaced. Default: the executable's folder.
    std::filesystem::path install_dir;
    /// Started after installing. Default: the running executable.
    std::filesystem::path relaunch_executable;
    std::vector<std::wstring> relaunch_args;
    /// msiexec UI switch: "/qn" (silent; right for per-user MSIs) or "/passive" (progress + UAC for per-machine).
    std::wstring msi_ui = L"/qn";
    /// When non-empty, Msi / Exe packages must also carry a valid Authenticode signature whose signer
    /// name contains this text.
    std::wstring authenticode_subject;
    /// Executable started as the install helper. Default: a copy of the running executable, which must
    /// then call Updater::run_helper_if_requested() at the top of wWinMain.
    std::filesystem::path helper_executable;
    HttpOptions http;
    std::shared_ptr<Logger> logger;
};

struct UpdateInfo {
    bool available = false;      ///< Newer and inside the rollout.
    bool newer = false;          ///< Newer than current_version, regardless of rollout.
    bool in_rollout = true;
    bool mandatory = false;
    std::string current_version;
    std::string version;
    std::string notes;
    PackageKind kind = PackageKind::Auto;
    UpdatePackage package;       ///< The selected artifact, url resolved.
    UpdateManifest manifest;
};

struct CheckResult {
    bool ok = false;
    std::string error;
    UpdateInfo info;
};

struct DownloadedUpdate {
    UpdateInfo info;
    std::filesystem::path package;     ///< Verified package file.
    std::filesystem::path staged_dir;  ///< Zip only: extracted files.
};

struct DownloadUpdateResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    DownloadedUpdate update;
};

struct InstallOptions {
    bool relaunch = true;
    std::vector<std::wstring> relaunch_args; ///< Appended to UpdaterConfig::relaunch_args.
    /// How long the helper waits for this process to exit. Negative = forever (install on next exit).
    std::chrono::milliseconds wait_timeout{-1};
    /// Process the helper waits for. Default: this process. (A launcher, or a test, can name another one.)
    std::optional<std::uint32_t> wait_pid;
};

/// @brief Outcome written by the helper; read on the next start with Updater::take_last_result().
struct ApplyResult {
    std::string version;
    std::string kind;
    bool success = false;
    bool rolled_back = false;
    std::string error;
    std::string finished_at;
    std::int64_t exit_code = 0;
};

/// @brief Fetches, verifies and installs updates.
///
/// check() / download() block and belong on background threads (TaskRunner::spawn, or the pool thread an
/// IPC controller is called on); check_async() / download_async() do that for you and complete on the UI
/// thread. schedule_install() is quick and starts a helper process that waits for this process to exit,
/// then installs (msiexec, installer exe, or a file swap with rollback) and optionally relaunches the app.
class Updater {
public:
    explicit Updater(UpdaterConfig config);
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    Updater(Updater&&) = delete;
    Updater& operator=(Updater&&) = delete;

    [[nodiscard]] CheckResult check(std::stop_token stop = {});
    [[nodiscard]] DownloadUpdateResult download(
        const UpdateInfo& info,
        const DownloadProgressFn& on_progress = {},
        std::stop_token stop = {});
    /// @brief SHA-256, ECDSA signature and (when configured) Authenticode checks of a package file.
    [[nodiscard]] bool verify_package(const UpdateInfo& info, const std::filesystem::path& package, std::string* error = nullptr) const;

    bool schedule_install(const DownloadedUpdate& update, const InstallOptions& options = {}, std::string* error = nullptr);
    bool cancel_scheduled_install() noexcept;
    [[nodiscard]] bool has_scheduled_install() const noexcept;
    /// @brief Waits for the helper started by schedule_install() to finish; its exit code (0 = installed).
    [[nodiscard]] std::optional<std::uint32_t> wait_for_scheduled_install(std::chrono::milliseconds timeout) const;

    /// @brief Reads and removes the result left by the last helper run.
    [[nodiscard]] std::optional<ApplyResult> take_last_result();

    /// @brief Cancels a running check / download started through this Updater.
    void cancel() noexcept;

    void check_async(exec::TaskRunner& runner, exec::UiMarshal& marshal, std::function<void(CheckResult)> on_done);
    void download_async(
        exec::TaskRunner& runner,
        exec::UiMarshal& marshal,
        UpdateInfo info,
        DownloadProgressFn on_progress,
        std::function<void(DownloadUpdateResult)> on_done);

    [[nodiscard]] const UpdaterConfig& config() const noexcept;
    [[nodiscard]] std::filesystem::path work_dir() const;

    /// @brief Call first thing in wWinMain: when this process is an install helper, runs the job and
    /// returns its exit code (return it from wWinMain); otherwise std::nullopt.
    [[nodiscard]] static std::optional<int> run_helper_if_requested();
    [[nodiscard]] static int run_helper(const std::filesystem::path& job_file);

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

struct PortableApplyResult {
    bool ok = false;
    bool rolled_back = false;
    std::size_t files_replaced = 0;
    std::string error;
};

/// @brief Copies every file of staged_dir over install_dir. Replaced files are first moved into
/// backup_dir; on any failure everything is put back. Locked files are retried until lock_timeout.
[[nodiscard]] PortableApplyResult apply_portable_update(
    const std::filesystem::path& staged_dir,
    const std::filesystem::path& install_dir,
    const std::filesystem::path& backup_dir,
    std::chrono::milliseconds lock_timeout = std::chrono::seconds{20});

/// @brief Extracts a .zip with the tar.exe that ships in System32 (Windows 10 1803+). Absolute paths and
/// ".." entries are refused by tar. Returns the content root: dest, or its only sub-folder when the
/// archive wraps everything in one top-level directory.
[[nodiscard]] std::optional<std::filesystem::path> extract_zip(
    const std::filesystem::path& zip,
    const std::filesystem::path& dest,
    std::string* error = nullptr);

/// @brief true when this process runs with MSIX package identity (such apps update through App Installer
/// or the Store, not through Updater).
[[nodiscard]] bool is_packaged_process() noexcept;

} // namespace boson::shipping
