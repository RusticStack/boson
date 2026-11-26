#include <boson/shipping/updater.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/shipping/authenticode.hpp>
#include <boson/shipping/crypto.hpp>
#include <boson/shipping/version.hpp>

#include "file_util.hpp"

#include <appmodel.h>
#include <sddl.h>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <system_error>

namespace boson::shipping {

namespace detail {

/// Serialized hand-over from the app to the install helper process.
struct InstallJob {
    std::string version;
    std::string kind;
    std::string package;
    std::string staged_dir;
    std::string install_dir;
    std::string backup_dir;
    std::uint32_t wait_pid = 0;
    std::int64_t wait_timeout_ms = -1;
    bool relaunch = true;
    std::string relaunch_executable;
    std::vector<std::string> relaunch_args;
    std::string msi_ui = "/qn";
    std::string msi_log;
    std::string installer_args;
    std::string result_file;
    std::string log_dir;
};

} // namespace detail

namespace {

using detail::InstallJob;
using detail::UniqueHandle;
using detail::make_handle;

constexpr std::string_view kCategory = "updater";

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string utf8(const std::filesystem::path& p) {
    return core::wide_to_utf8(p.wstring());
}

std::filesystem::path from_utf8(std::string_view s) {
    return std::filesystem::path{core::utf8_to_wide(s)};
}

bool is_sha256_hex(std::string_view s) {
    return s.size() == 64 && from_hex(s).has_value();
}

std::string utc_now_iso() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

/// Runs command_line hidden and waits; exit code or nullopt when it could not start / timed out.
std::optional<DWORD> run_and_wait(std::wstring command_line, DWORD timeout_ms, std::string* error) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (error != nullptr) {
            *error = "CreateProcess failed (" + detail::win32_message(GetLastError()) + ")";
        }
        return std::nullopt;
    }
    const UniqueHandle process(pi.hProcess);
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(process.get(), timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(process.get(), ERROR_TIMEOUT);
        if (error != nullptr) {
            *error = "process timed out";
        }
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    return code;
}

std::filesystem::path system_directory() {
    wchar_t buffer[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(buffer, MAX_PATH);
    return std::filesystem::path{std::wstring_view{buffer, len}};
}

std::string file_name_from_url(std::string_view url) {
    std::string_view path = url;
    if (const auto q = path.find_first_of("?#"); q != std::string_view::npos) {
        path = path.substr(0, q);
    }
    if (const auto slash = path.find_last_of("/\\"); slash != std::string_view::npos) {
        path = path.substr(slash + 1);
    }
    std::string name;
    for (const char ch : path) {
        const bool safe = std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '_';
        name.push_back(safe ? ch : '_');
    }
    if (name.empty() || name == "." || name == "..") {
        name = "package.bin";
    }
    return name;
}

/// stop_source that fires when either of two tokens does.
class LinkedStop {
public:
    LinkedStop(std::stop_token a, std::stop_token b)
        : m_a(std::move(a), Fire{&m_source})
        , m_b(std::move(b), Fire{&m_source}) {}

    [[nodiscard]] std::stop_token token() const noexcept { return m_source.get_token(); }

private:
    struct Fire {
        std::stop_source* source;
        void operator()() const noexcept { source->request_stop(); }
    };
    std::stop_source m_source;
    std::stop_callback<Fire> m_a;
    std::stop_callback<Fire> m_b;
};

} // namespace

// ---- Free functions -----------------------------------------------------------------------------------

std::string_view to_string(PackageKind kind) noexcept {
    switch (kind) {
    case PackageKind::Auto: return "auto";
    case PackageKind::Msi: return "msi";
    case PackageKind::Zip: return "zip";
    case PackageKind::Exe: return "exe";
    }
    return "auto";
}

PackageKind parse_package_kind(std::string_view name) noexcept {
    std::string value;
    for (const char ch : name.substr(0, 8)) {
        value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (value == "msi") return PackageKind::Msi;
    if (value == "zip" || value == "portable") return PackageKind::Zip;
    if (value == "exe" || value == "installer") return PackageKind::Exe;
    return PackageKind::Auto;
}

PackageKind package_kind_from_url(std::string_view url) noexcept {
    if (const auto q = url.find_first_of("?#"); q != std::string_view::npos) {
        url = url.substr(0, q);
    }
    const auto dot = url.find_last_of('.');
    if (dot == std::string_view::npos || url.find('/', dot) != std::string_view::npos) {
        return PackageKind::Auto;
    }
    return parse_package_kind(url.substr(dot + 1));
}

std::string update_signing_payload(std::string_view version, std::string_view sha256_hex) {
    std::string out = "boson-update-v1\n";
    out += version;
    out.push_back('\n');
    out += lower_ascii(sha256_hex);
    out.push_back('\n');
    return out;
}

std::uint32_t rollout_bucket(std::string_view install_id, std::string_view version) {
    std::string input(install_id);
    input.push_back('\n');
    input += version;
    const auto digest = sha256(input);
    if (!digest) {
        return 0;
    }
    const std::uint32_t value = (static_cast<std::uint32_t>((*digest)[0]) << 24) | (static_cast<std::uint32_t>((*digest)[1]) << 16) |
                                (static_cast<std::uint32_t>((*digest)[2]) << 8) | static_cast<std::uint32_t>((*digest)[3]);
    return value % 10000u;
}

bool in_rollout(double percentage, std::string_view install_id, std::string_view version) {
    if (!(percentage > 0.0)) {
        return false;
    }
    if (percentage >= 100.0) {
        return true;
    }
    return static_cast<double>(rollout_bucket(install_id, version)) < percentage * 100.0;
}

std::string default_install_id() {
    std::string material = "boson-install-id\n";
    wchar_t guid[64]{};
    DWORD size = sizeof(guid);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Cryptography",
            L"MachineGuid",
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
            nullptr,
            guid,
            &size) == ERROR_SUCCESS) {
        material += core::wide_to_utf8(guid);
    }
    material.push_back('\n');
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        const UniqueHandle token_guard(token);
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        std::vector<std::uint8_t> buffer(needed);
        if (needed > 0 && GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
            const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
                material += core::wide_to_utf8(sid);
                LocalFree(sid);
            }
        }
    }
    const auto digest = sha256(material);
    return digest ? to_hex(*digest) : std::string{"anonymous"};
}

ManifestParseResult parse_update_manifest(std::string_view json, std::string_view manifest_url) {
    ManifestParseResult result;
    UpdateManifest manifest;
    const std::string buffer(json);
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(manifest, buffer)) {
        result.error = "invalid manifest JSON: " + glz::format_error(ec, buffer);
        return result;
    }
    if (!SemVer::parse(manifest.version)) {
        result.error = "manifest version \"" + manifest.version + "\" is not a valid version";
        return result;
    }
    if (!manifest.minimum_version.empty() && !SemVer::parse(manifest.minimum_version)) {
        result.error = "manifest minimum_version is not a valid version";
        return result;
    }
    if (!std::isfinite(manifest.rollout) || manifest.rollout < 0.0 || manifest.rollout > 100.0) {
        result.error = "manifest rollout must be a percentage between 0 and 100";
        return result;
    }
    if (manifest.url.empty() && manifest.packages.empty()) {
        result.error = "manifest has neither \"url\" nor \"packages\"";
        return result;
    }
    const auto validate = [&](std::string& url, const std::string& hash, std::string_view label) {
        if (url.empty()) {
            result.error = std::string(label) + ": missing url";
            return false;
        }
        if (!is_sha256_hex(hash)) {
            result.error = std::string(label) + ": sha256 must be 64 hex characters";
            return false;
        }
        if (!manifest_url.empty()) {
            url = resolve_url(manifest_url, url);
        }
        return true;
    };
    if (!manifest.url.empty() && !validate(manifest.url, manifest.sha256, "manifest")) {
        return result;
    }
    for (auto& [name, package] : manifest.packages) {
        if (!validate(package.url, package.sha256, "packages." + name)) {
            return result;
        }
    }
    if (!manifest.kind.empty() && parse_package_kind(manifest.kind) == PackageKind::Auto) {
        result.error = "manifest kind must be \"msi\", \"zip\" or \"exe\"";
        return result;
    }
    result.manifest = std::move(manifest);
    return result;
}

bool is_packaged_process() noexcept {
    UINT32 length = 0;
    const LONG rc = GetCurrentPackageFullName(&length, nullptr);
    return rc != APPMODEL_ERROR_NO_PACKAGE;
}

std::optional<std::filesystem::path> extract_zip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string* error) {
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "cannot create " + utf8(dest);
        }
        return std::nullopt;
    }
    const std::filesystem::path tar = system_directory() / L"tar.exe";
    const std::vector<std::wstring> args = {L"-xf", zip.wstring(), L"-C", dest.wstring()};
    std::string run_error;
    const auto code = run_and_wait(app::build_command_line(tar.wstring(), args), 10 * 60 * 1000, &run_error);
    if (!code.has_value() || *code != 0) {
        if (error != nullptr) {
            *error = code.has_value() ? "tar.exe failed with exit code " + std::to_string(*code) : run_error;
        }
        return std::nullopt;
    }
    std::filesystem::path only;
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dest, ec)) {
        ++entries;
        only = entry.path();
    }
    if (entries == 0) {
        if (error != nullptr) {
            *error = "archive is empty";
        }
        return std::nullopt;
    }
    if (entries == 1 && std::filesystem::is_directory(only, ec)) {
        return only;
    }
    return dest;
}

PortableApplyResult apply_portable_update(
    const std::filesystem::path& staged_dir,
    const std::filesystem::path& install_dir,
    const std::filesystem::path& backup_dir,
    std::chrono::milliseconds lock_timeout) {
    PortableApplyResult result;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(staged_dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(std::filesystem::relative(it->path(), staged_dir, ec));
        }
    }
    if (ec || files.empty()) {
        result.error = "staged update folder is empty or unreadable";
        return result;
    }
    std::filesystem::remove_all(backup_dir, ec);
    std::filesystem::create_directories(backup_dir, ec);

    struct Done {
        std::filesystem::path rel;
        bool had_original = false;
    };
    std::vector<Done> done;
    const auto deadline = std::chrono::steady_clock::now() + lock_timeout;
    // Retries while a file is still locked (the exiting app, an AV scanner, the search indexer).
    const auto retry = [&](auto&& op) {
        while (true) {
            if (op()) {
                return true;
            }
            const DWORD code = GetLastError();
            const bool transient = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION;
            if (!transient || std::chrono::steady_clock::now() >= deadline) {
                SetLastError(code);
                return false;
            }
            Sleep(100);
        }
    };

    for (const auto& rel : files) {
        const auto source = staged_dir / rel;
        const auto target = install_dir / rel;
        const auto backup = backup_dir / rel;
        std::filesystem::create_directories(target.parent_path(), ec);
        Done entry{.rel = rel};
        if (std::filesystem::exists(target, ec)) {
            std::filesystem::create_directories(backup.parent_path(), ec);
            // A rename works even for a loaded DLL / running EXE on the same volume; copying would not.
            if (!retry([&]() { return MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE; })) {
                result.error = "cannot move " + utf8(target) + " aside (" + detail::win32_message(GetLastError()) + ")";
                break;
            }
            entry.had_original = true;
        }
        done.push_back(entry);
        if (!retry([&]() { return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE; })) {
            result.error = "cannot write " + utf8(target) + " (" + detail::win32_message(GetLastError()) + ")";
            break;
        }
        ++result.files_replaced;
    }

    if (result.error.empty()) {
        result.ok = true;
        std::filesystem::remove_all(backup_dir, ec);
        return result;
    }

    // Roll back in reverse order: remove what was written, move the originals back.
    bool restored = true;
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        const auto target = install_dir / it->rel;
        const auto backup = backup_dir / it->rel;
        DeleteFileW(target.c_str());
        if (it->had_original && !MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            restored = false;
        }
    }
    result.rolled_back = restored;
    if (restored) {
        std::filesystem::remove_all(backup_dir, ec);
    } else {
        result.error += "; rollback incomplete, originals kept in " + utf8(backup_dir);
    }
    result.files_replaced = 0;
    return result;
}

// ---- Updater ------------------------------------------------------------------------------------------

struct Updater::Impl {
    UpdaterConfig config;
    std::filesystem::path work_dir;
    mutable std::mutex mutex;
    std::stop_source stop;
    UniqueHandle helper_process;
    std::filesystem::path job_file;

    void log(LogLevel level, std::string_view message) const {
        log_message(config.logger, level, kCategory, message);
    }

    std::stop_token token() const {
        std::scoped_lock lock(mutex);
        return stop.get_token();
    }

    CheckResult check(std::stop_token user_stop) const {
        CheckResult result;
        const LinkedStop linked(user_stop, token());
        if (config.manifest_url.empty()) {
            result.error = "UpdaterConfig::manifest_url is empty";
            return result;
        }
        const auto current = SemVer::parse(config.current_version);
        if (!current) {
            result.error = "UpdaterConfig::current_version \"" + config.current_version + "\" is not a valid version";
            return result;
        }
        log(LogLevel::Info, "checking " + config.manifest_url);
        const HttpResponse response = http_get(config.manifest_url, config.http, linked.token());
        if (!response.ok) {
            result.error = "manifest download failed: " + response.error;
            log(LogLevel::Warn, result.error);
            return result;
        }
        auto parsed = parse_update_manifest(response.body, response.final_url.empty() ? config.manifest_url : response.final_url);
        if (!parsed.manifest) {
            result.error = parsed.error;
            log(LogLevel::Warn, result.error);
            return result;
        }
        UpdateManifest& manifest = *parsed.manifest;
        UpdateInfo& info = result.info;
        info.current_version = config.current_version;
        info.version = manifest.version;
        info.notes = manifest.notes;

        // Package selection: the entry for this install kind, else the top-level package.
        PackageKind kind = PackageKind::Auto;
        if (config.install_kind != PackageKind::Auto) {
            if (const auto it = manifest.packages.find(std::string(to_string(config.install_kind))); it != manifest.packages.end()) {
                info.package = it->second;
                kind = config.install_kind;
            }
        }
        if (info.package.url.empty()) {
            if (!manifest.url.empty()) {
                info.package = UpdatePackage{
                    .url = manifest.url,
                    .sha256 = manifest.sha256,
                    .signature = manifest.signature,
                    .size = manifest.size,
                    .installer_args = manifest.installer_args,
                };
                kind = parse_package_kind(manifest.kind);
            } else if (manifest.packages.size() == 1) {
                info.package = manifest.packages.begin()->second;
                kind = parse_package_kind(manifest.packages.begin()->first);
            } else {
                result.error = "manifest has no package for install kind \"" + std::string(to_string(config.install_kind)) + "\"";
                return result;
            }
        }
        if (kind == PackageKind::Auto) {
            kind = package_kind_from_url(info.package.url);
        }
        if (kind == PackageKind::Auto) {
            kind = config.install_kind;
        }
        info.kind = kind;
        if (const std::string refused = check_url_allowed(info.package.url, config.http); !refused.empty()) {
            result.error = "package URL refused: " + refused;
            return result;
        }

        const auto candidate = SemVer::parse(manifest.version);
        info.newer = *candidate > *current;
        const auto minimum = SemVer::parse(manifest.minimum_version);
        info.mandatory = manifest.mandatory || (minimum.has_value() && *current < *minimum);
        const std::string install_id = config.install_id.empty() ? default_install_id() : config.install_id;
        info.in_rollout = info.mandatory || in_rollout(manifest.rollout, install_id, manifest.version);
        info.available = info.newer && info.in_rollout;
        info.manifest = std::move(manifest);
        result.ok = true;
        log(LogLevel::Info,
            "manifest version " + info.version + " (current " + info.current_version + "): " +
                (info.available ? "update available" : info.newer ? "newer, outside rollout" : "up to date"));
        return result;
    }

    bool verify(const UpdateInfo& info, const std::filesystem::path& package, std::string* error) const {
        const auto fail = [&](std::string message) {
            log(LogLevel::Error, "verification failed: " + message);
            if (error != nullptr) {
                *error = std::move(message);
            }
            return false;
        };
        std::error_code ec;
        if (info.package.size.has_value() && std::filesystem::file_size(package, ec) != *info.package.size) {
            return fail("size mismatch");
        }
        const auto digest = sha256_file(package);
        if (!digest) {
            return fail("cannot hash " + utf8(package));
        }
        const std::string actual = to_hex(*digest);
        if (!hex_equals(actual, info.package.sha256)) {
            return fail("SHA-256 mismatch (expected " + info.package.sha256 + ", got " + actual + ")");
        }
        if (config.public_key.empty()) {
            return fail("UpdaterConfig::public_key is empty: unsigned updates are never installed");
        }
        const auto key = base64_decode(config.public_key);
        if (!key || key->size() != kEcdsaP256PublicKeySize) {
            return fail("UpdaterConfig::public_key is not a base64 64-byte P-256 public key");
        }
        const auto signature = base64_decode(info.package.signature);
        if (!signature || signature->size() != kEcdsaP256SignatureSize) {
            return fail("package signature is missing or malformed");
        }
        const std::string payload = update_signing_payload(info.version, actual);
        if (!ecdsa_p256_verify(*key, as_bytes(payload), *signature)) {
            return fail("ECDSA signature does not verify");
        }
        if (!config.authenticode_subject.empty() && (info.kind == PackageKind::Msi || info.kind == PackageKind::Exe)) {
            const AuthenticodeResult authenticode = verify_authenticode(package, config.authenticode_subject);
            if (!authenticode.ok()) {
                return fail("Authenticode: " + authenticode.error);
            }
        }
        return true;
    }

    DownloadUpdateResult download(const UpdateInfo& info, const DownloadProgressFn& on_progress, std::stop_token user_stop) const {
        DownloadUpdateResult result;
        result.update.info = info;
        if (info.package.url.empty()) {
            result.error = "no package to download";
            return result;
        }
        if (info.kind == PackageKind::Auto) {
            result.error = "cannot tell whether the package is an MSI, ZIP or EXE (set \"kind\" in the manifest)";
            return result;
        }
        const LinkedStop linked(user_stop, token());
        const auto dir = work_dir / L"downloads";
        const auto destination = dir / core::utf8_to_wide(info.version + "-" + file_name_from_url(info.package.url));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        bool have = false;
        if (std::filesystem::exists(destination, ec)) {
            have = verify(info, destination, nullptr);
            if (!have) {
                DeleteFileW(destination.c_str());
            }
        }
        if (!have) {
            log(LogLevel::Info, "downloading " + info.package.url);
            DownloadOptions options;
            options.expected_size = info.package.size;
            const DownloadResult downloaded = download_file(info.package.url, destination, config.http, on_progress, linked.token(), options);
            if (!downloaded.ok) {
                result.cancelled = downloaded.cancelled;
                result.error = "download failed: " + downloaded.error;
                log(downloaded.cancelled ? LogLevel::Info : LogLevel::Warn, result.error);
                return result;
            }
            if (!verify(info, destination, &result.error)) {
                DeleteFileW(destination.c_str());
                return result;
            }
        } else if (on_progress) {
            const auto size = std::filesystem::file_size(destination, ec);
            on_progress(DownloadProgress{.received = size, .total = size, .resumed_from = size});
        }
        result.update.package = destination;

        if (info.kind == PackageKind::Zip) {
            const auto staged = extract_zip(destination, work_dir / L"staged" / core::utf8_to_wide(info.version), &result.error);
            if (!staged) {
                result.error = "extract failed: " + result.error;
                log(LogLevel::Error, result.error);
                return result;
            }
            result.update.staged_dir = *staged;
        }
        log(LogLevel::Info, "update " + info.version + " downloaded and verified");
        result.ok = true;
        return result;
    }
};

Updater::Updater(UpdaterConfig config)
    : m_impl(std::make_shared<Impl>()) {
    m_impl->config = std::move(config);
    m_impl->work_dir = m_impl->config.work_dir.empty()
                           ? detail::known_local_app_data() / m_impl->config.app_id / L"updates"
                           : m_impl->config.work_dir;
}

Updater::~Updater() {
    cancel();
}

const UpdaterConfig& Updater::config() const noexcept {
    return m_impl->config;
}

std::filesystem::path Updater::work_dir() const {
    return m_impl->work_dir;
}

void Updater::cancel() noexcept {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->stop.request_stop();
    m_impl->stop = std::stop_source{};
}

CheckResult Updater::check(std::stop_token stop) {
    try {
        return m_impl->check(std::move(stop));
    } catch (const std::exception& ex) {
        return CheckResult{.error = ex.what()};
    }
}

DownloadUpdateResult Updater::download(const UpdateInfo& info, const DownloadProgressFn& on_progress, std::stop_token stop) {
    try {
        return m_impl->download(info, on_progress, std::move(stop));
    } catch (const std::exception& ex) {
        return DownloadUpdateResult{.error = ex.what()};
    }
}

bool Updater::verify_package(const UpdateInfo& info, const std::filesystem::path& package, std::string* error) const {
    return m_impl->verify(info, package, error);
}

void Updater::check_async(exec::TaskRunner& runner, exec::UiMarshal& marshal, std::function<void(CheckResult)> on_done) {
    runner.spawn([impl = m_impl, &marshal, done = std::move(on_done)]() mutable {
        CheckResult result;
        try {
            result = impl->check({});
        } catch (const std::exception& ex) {
            result.error = ex.what();
        }
        if (done) {
            marshal.post_task([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
        }
    });
}

void Updater::download_async(
    exec::TaskRunner& runner,
    exec::UiMarshal& marshal,
    UpdateInfo info,
    DownloadProgressFn on_progress,
    std::function<void(DownloadUpdateResult)> on_done) {
    runner.spawn([impl = m_impl, &marshal, info = std::move(info), progress = std::move(on_progress), done = std::move(on_done)]() mutable {
        DownloadUpdateResult result;
        try {
            result = impl->download(info, progress, {});
        } catch (const std::exception& ex) {
            result.error = ex.what();
        }
        if (done) {
            marshal.post_task([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
        }
    });
}

bool Updater::schedule_install(const DownloadedUpdate& update, const InstallOptions& options, std::string* error) {
    const auto fail = [&](std::string message) {
        m_impl->log(LogLevel::Error, "schedule_install: " + message);
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    const UpdaterConfig& config = m_impl->config;
    const PackageKind kind = update.info.kind;
    if (kind == PackageKind::Auto) {
        return fail("unknown package kind");
    }
    if (update.package.empty() || !std::filesystem::exists(update.package)) {
        return fail("the downloaded package is missing");
    }
    // Re-verify right before handing the file to an installer: the download folder is user-writable.
    std::string verify_error;
    if (!m_impl->verify(update.info, update.package, &verify_error)) {
        return fail(verify_error);
    }
    if (kind == PackageKind::Zip && update.staged_dir.empty()) {
        return fail("a zip update needs its staged folder");
    }
    cancel_scheduled_install();

    const std::wstring exe = app::executable_path();
    const std::filesystem::path exe_path{exe};
    std::error_code ec;
    const auto work = m_impl->work_dir;
    std::filesystem::create_directories(work / L"jobs", ec);
    std::filesystem::create_directories(work / L"helper", ec);
    std::filesystem::create_directories(work / L"logs", ec);

    const std::filesystem::path install_dir = config.install_dir.empty() ? exe_path.parent_path() : config.install_dir;
    InstallJob job;
    job.version = update.info.version;
    job.kind = std::string(to_string(kind));
    job.package = utf8(update.package);
    job.staged_dir = utf8(update.staged_dir);
    job.install_dir = utf8(install_dir);
    // Inside the install folder, so originals are renamed (same volume) rather than copied.
    job.backup_dir = utf8(install_dir / L".boson-update-backup");
    job.wait_pid = options.wait_pid.value_or(GetCurrentProcessId());
    job.wait_timeout_ms = options.wait_timeout.count();
    job.relaunch = options.relaunch;
    job.relaunch_executable = utf8(config.relaunch_executable.empty() ? exe_path : config.relaunch_executable);
    for (const auto& arg : config.relaunch_args) {
        job.relaunch_args.push_back(core::wide_to_utf8(arg));
    }
    for (const auto& arg : options.relaunch_args) {
        job.relaunch_args.push_back(core::wide_to_utf8(arg));
    }
    job.msi_ui = core::wide_to_utf8(config.msi_ui);
    job.msi_log = utf8(work / L"logs" / core::utf8_to_wide("msiexec-" + update.info.version + ".log"));
    job.installer_args = update.info.package.installer_args;
    job.result_file = utf8(work / L"last-result.json");
    job.log_dir = utf8(work / L"logs");

    std::string job_json;
    if (glz::write_json(job, job_json)) {
        return fail("cannot serialize the install job");
    }
    const auto job_file = work / L"jobs" / (L"install-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    if (!detail::write_text_file(job_file, job_json)) {
        return fail("cannot write " + utf8(job_file));
    }

    // The helper runs from a copy, so a portable update can replace the original executable.
    const std::filesystem::path helper_source = config.helper_executable.empty() ? exe_path : config.helper_executable;
    for (const auto& entry : std::filesystem::directory_iterator(work / L"helper", ec)) {
        std::filesystem::remove(entry.path(), ec); // stale copies; ones still running stay locked, which is fine
    }
    const auto helper = work / L"helper" /
                        (helper_source.stem().wstring() + L"-update-helper-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    if (!CopyFileW(helper_source.c_str(), helper.c_str(), FALSE)) {
        return fail("cannot copy the helper executable (" + detail::win32_message(GetLastError()) + ")");
    }

    const std::vector<std::wstring> args = {std::wstring(kUpdateHelperSwitch) + job_file.wstring()};
    std::wstring command_line = app::build_command_line(helper.wstring(), args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Break away from a kill-on-close job (terminals, some launchers) so the helper outlives the app.
    BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW,
                                  nullptr, helper.parent_path().c_str(), &si, &pi);
    if (!started) {
        started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 helper.parent_path().c_str(), &si, &pi);
    }
    if (!started) {
        DeleteFileW(job_file.c_str());
        return fail("cannot start the install helper (" + detail::win32_message(GetLastError()) + ")");
    }
    CloseHandle(pi.hThread);
    {
        std::scoped_lock lock(m_impl->mutex);
        m_impl->helper_process.reset(pi.hProcess);
        m_impl->job_file = job_file;
    }
    m_impl->log(LogLevel::Info, "install of " + update.info.version + " (" + job.kind + ") scheduled; helper pid " + std::to_string(pi.dwProcessId));
    return true;
}

bool Updater::cancel_scheduled_install() noexcept {
    std::scoped_lock lock(m_impl->mutex);
    if (!m_impl->helper_process) {
        return false;
    }
    bool cancelled = false;
    if (WaitForSingleObject(m_impl->helper_process.get(), 0) == WAIT_TIMEOUT) {
        cancelled = TerminateProcess(m_impl->helper_process.get(), ERROR_CANCELLED) != FALSE;
        WaitForSingleObject(m_impl->helper_process.get(), 5000);
    }
    m_impl->helper_process.reset();
    if (!m_impl->job_file.empty()) {
        DeleteFileW(m_impl->job_file.c_str());
        m_impl->job_file.clear();
    }
    return cancelled;
}

bool Updater::has_scheduled_install() const noexcept {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->helper_process && WaitForSingleObject(m_impl->helper_process.get(), 0) == WAIT_TIMEOUT;
}

std::optional<std::uint32_t> Updater::wait_for_scheduled_install(std::chrono::milliseconds timeout) const {
    HANDLE process = nullptr;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (!m_impl->helper_process ||
            !DuplicateHandle(GetCurrentProcess(), m_impl->helper_process.get(), GetCurrentProcess(), &process, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
            return std::nullopt;
        }
    }
    const UniqueHandle guard(process);
    const DWORD wait = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(timeout.count());
    if (WaitForSingleObject(process, wait) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    return code;
}

std::optional<ApplyResult> Updater::take_last_result() {
    const auto path = m_impl->work_dir / L"last-result.json";
    const auto text = detail::read_text_file(path);
    if (!text) {
        return std::nullopt;
    }
    DeleteFileW(path.c_str());
    ApplyResult result;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(result, *text)) {
        return std::nullopt;
    }
    return result;
}

std::optional<int> Updater::run_helper_if_requested() {
    for (const auto& arg : app::command_line_args()) {
        if (std::wstring_view{arg}.starts_with(kUpdateHelperSwitch)) {
            return run_helper(std::filesystem::path{std::wstring_view{arg}.substr(kUpdateHelperSwitch.size())});
        }
    }
    return std::nullopt;
}

int Updater::run_helper(const std::filesystem::path& job_file) {
    const auto text = detail::read_text_file(job_file);
    InstallJob job;
    if (!text || glz::read<glz::opts{.error_on_unknown_keys = false}>(job, *text)) {
        return 2;
    }
    std::shared_ptr<Logger> logger;
    if (!job.log_dir.empty()) {
        logger = std::make_shared<Logger>(LoggerConfig{
            .directory = from_utf8(job.log_dir),
            .file_name = L"update-helper.log",
            .max_file_bytes = 1024 * 1024,
            .max_files = 3,
            .min_level = LogLevel::Info,
        });
    }
    const auto log = [&](LogLevel level, std::string_view message) { log_message(logger, level, kCategory, message); };
    log(LogLevel::Info, "helper started for " + job.version + " (" + job.kind + "), waiting for pid " + std::to_string(job.wait_pid));

    ApplyResult result;
    result.version = job.version;
    result.kind = job.kind;

    if (job.wait_pid != 0 && job.wait_pid != GetCurrentProcessId()) {
        const UniqueHandle target(OpenProcess(SYNCHRONIZE, FALSE, job.wait_pid));
        if (target) {
            const DWORD wait = job.wait_timeout_ms < 0 ? INFINITE : static_cast<DWORD>(std::min<std::int64_t>(job.wait_timeout_ms, 0x7FFFFFFF));
            if (WaitForSingleObject(target.get(), wait) != WAIT_OBJECT_0) {
                result.error = "timed out waiting for the application to exit";
            }
        }
    }

    if (result.error.empty()) {
        const PackageKind kind = parse_package_kind(job.kind);
        const auto package = from_utf8(job.package);
        if (kind == PackageKind::Msi) {
            const std::filesystem::path msiexec = system_directory() / L"msiexec.exe";
            std::vector<std::wstring> args = {L"/i", package.wstring()};
            args.push_back(core::utf8_to_wide(job.msi_ui.empty() ? std::string{"/qn"} : job.msi_ui));
            args.push_back(L"/norestart");
            args.push_back(L"/l*v");
            args.push_back(from_utf8(job.msi_log).wstring());
            std::string run_error;
            const auto code = run_and_wait(app::build_command_line(msiexec.wstring(), args), 60 * 60 * 1000, &run_error);
            result.exit_code = code.value_or(0);
            // 3010 / 1641: installed, reboot required / initiated. msiexec rolls back failed installs itself.
            result.success = code.has_value() && (*code == ERROR_SUCCESS || *code == ERROR_SUCCESS_REBOOT_REQUIRED || *code == ERROR_SUCCESS_REBOOT_INITIATED);
            if (!result.success) {
                result.error = code.has_value() ? "msiexec failed with exit code " + std::to_string(*code) + " (log: " + job.msi_log + ")" : run_error;
                result.rolled_back = code.has_value();
            }
        } else if (kind == PackageKind::Exe) {
            std::wstring command_line = L"\"" + package.wstring() + L"\"";
            if (!job.installer_args.empty()) {
                command_line += L" " + core::utf8_to_wide(job.installer_args);
            }
            std::string run_error;
            const auto code = run_and_wait(std::move(command_line), 60 * 60 * 1000, &run_error);
            result.exit_code = code.value_or(0);
            result.success = code.has_value() && *code == 0;
            if (!result.success) {
                result.error = code.has_value() ? "installer failed with exit code " + std::to_string(*code) : run_error;
            }
        } else if (kind == PackageKind::Zip) {
            const PortableApplyResult applied = apply_portable_update(from_utf8(job.staged_dir), from_utf8(job.install_dir), from_utf8(job.backup_dir));
            result.success = applied.ok;
            result.rolled_back = applied.rolled_back;
            result.error = applied.error;
            if (applied.ok) {
                std::error_code ec;
                std::filesystem::remove_all(from_utf8(job.staged_dir), ec);
                log(LogLevel::Info, std::to_string(applied.files_replaced) + " files replaced");
            }
        } else {
            result.error = "unknown package kind \"" + job.kind + "\"";
        }
    }

    result.finished_at = utc_now_iso();
    log(result.success ? LogLevel::Info : LogLevel::Error,
        result.success ? "update " + job.version + " installed" : "update " + job.version + " failed: " + result.error);
    std::string result_json;
    if (!glz::write_json(result, result_json) && !job.result_file.empty()) {
        detail::write_text_file(from_utf8(job.result_file), result_json);
    }
    if (result.success && !job.package.empty()) {
        DeleteFileW(from_utf8(job.package).c_str());
    }
    DeleteFileW(job_file.c_str());

    if (job.relaunch && !job.relaunch_executable.empty()) {
        std::vector<std::wstring> args;
        for (const auto& arg : job.relaunch_args) {
            args.push_back(core::utf8_to_wide(arg));
        }
        args.push_back(std::wstring(result.success ? kUpdatedSwitch : kUpdateFailedSwitch) + core::utf8_to_wide(job.version));
        std::wstring command_line = app::build_command_line(from_utf8(job.relaunch_executable).wstring(), args);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const auto exe_dir = from_utf8(job.relaunch_executable).parent_path();
        if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, exe_dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            log(LogLevel::Error, "relaunch failed (" + detail::win32_message(GetLastError()) + ")");
        }
    }
    if (logger) {
        logger->flush();
    }
    return result.success ? 0 : 1;
}

} // namespace boson::shipping
