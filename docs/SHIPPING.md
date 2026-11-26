# Shipping: packaging, signing, updates, crash reports, logging

Everything here is Windows 10/11 only and uses what Windows, the Windows SDK and the existing dependencies
(glaze, stdexec, WebView2 SDK) provide: WinHTTP, CNG (BCrypt), WinTrust, DbgHelp, signtool, makeappx and CPack.

| Area | Files |
|---|---|
| Packaging (ZIP, MSI, MSIX) | `cmake/BosonPackaging.cmake` (`boson_package()`), `cmake/templates/*`, `cmake/msix/generate_assets.ps1` |
| Code signing | `cmake/BosonSign.cmake`, `cmake/BosonCPackPostBuild.cmake`, `boson_sign_target()`, `boson_sign_command()` |
| Auto-updater | `boson/shipping/updater.hpp`, `boson/ipc/controllers/updater_controller.hpp`, `tools/updater-sign` |
| HTTP / downloads | `boson/shipping/http.hpp` |
| Crypto (SHA-256, ECDSA P-256, base64) | `boson/shipping/crypto.hpp` |
| Versions | `boson/shipping/version.hpp` |
| WebView2 runtime bootstrapper | `boson/shipping/webview2_runtime.hpp` |
| Authenticode verification | `boson/shipping/authenticode.hpp` |
| Crash reporting | `boson/shipping/crash_reporter.hpp` |
| Logging | `boson/shipping/logger.hpp`, `boson/ipc/controllers/log_controller.hpp` |
| Frontend helpers | `frontend/src/updater.ts` (`updater.*`, `log()`) |

## Tools found on the development machine

- Windows SDK 10.0.26100.0: `makeappx.exe`, `signtool.exe`, `mt.exe`, `makepri.exe` (x64): **present**.
- CMake / CPack 4.4.4: **present** (CPack WIX supports WiX v3 and, with `CPACK_WIX_VERSION 4`, WiX v4+).
- WiX Toolset (v3 `candle`/`light` or v4+ `wix.exe`): **not installed**, so MSIs could not be built here.
- `tar.exe` (System32, used to extract ZIP updates): present on Windows 10 1803+.

## 1. Packaging

```cmake
include(cmake/BosonPackaging.cmake)
boson_package(my_app
    NAME "My App" VERSION 1.4.0 VENDOR "Acme"
    DESCRIPTION "My App"
    APP_USER_MODEL_ID "Acme.MyApp"                         # shortcut AUMID (toasts, taskbar grouping)
    UPGRADE_GUID "7D9E5E2A-0000-4000-8000-000000000001"    # pin it: MSI major upgrades depend on it
    PROTOCOLS myapp                                         # myapp:// -> "my_app.exe" "%1"
    ICON "${CMAKE_SOURCE_DIR}/app.ico"
    LICENSE "${CMAKE_SOURCE_DIR}/LICENSE"
    CLEAN_APPDATA_DIR "My App"                              # %LOCALAPPDATA%\My App removed on MSI uninstall
    MSIX_IDENTITY "Acme.MyApp" MSIX_PUBLISHER "CN=Acme Corp")
boson_sign_target(my_app)                                  # signs after each build when -DBOSON_SIGN=ON
```

`boson_package()` can be called once per project (CPack has one configuration). It adds:

- `install()` rules: the executable, `EXTRA_FILES` / `EXTRA_DIRECTORIES` and the app-local MSVC runtime DLLs
  (`InstallRequiredSystemLibraries`; `NO_VC_RUNTIME` turns that off). Frontend assets are already `#embed`-ded.
- **Portable ZIP**: `cpack -G ZIP` -> `build/packages/<Name>-<ver>-win64.zip`.
- **MSI** (CPack WIX generator): `cpack -G WIX`. Per-user install (`CPACK_WIX_INSTALL_SCOPE perUser`, i.e. under
  `%LOCALAPPDATA%\Programs`) plus a WiX fragment (`build/boson_package/<target>/boson_wix_integration.wxs`, added to the
  root feature through `CPACK_WIX_PATCH_FILE` / `#PRODUCTFEATURE`) with: the Start-menu shortcut carrying
  `System.AppUserModel.ID`; `HKCU\Software\Classes\<scheme>` protocol keys (deleted on uninstall); an
  `HKCU\Software\<Vendor>\<Name>` key removed on uninstall; removal of `*.log` next to the exe; and, with
  `CLEAN_APPDATA_DIR`, a `util:RemoveFolderEx` of the app-data folder (the path is remembered in the registry at install
  and read back with `RegistrySearch`, the documented WiX pattern). Component GUIDs are derived deterministically from
  `UPGRADE_GUID`. WiX v3 or v4 syntax follows the installed WiX (or `WIX_VERSION`). The WIX generator is only part of
  a plain `cpack` run when WiX is found.
- **MSIX**: target `<target>_msix` stages a layout (exe, runtime DLLs, extra files, `Assets/` — placeholder logos are
  generated unless `MSIX_ASSETS` is given), writes `AppxManifest.xml` from `cmake/templates/AppxManifest.xml.in`
  (identity, publisher, four-part version, `runFullTrust`, `windows.protocol` extensions) and runs
  `makeappx pack /h SHA256`. An MSIX only installs when signed with a certificate whose subject equals
  `MSIX_PUBLISHER`; with `BOSON_SIGN=ON` the target signs it.

This repository packages `boson_demo` that way (`BOSON_PACKAGE_DEMO`, ON by default):

```
cmake --build build/wt --target boson_demo_msix     # build/wt/packages/Boson-Demo-0.1.0-win64.msix
cd build/wt && cpack -G ZIP                          # build/wt/packages/Boson-Demo-0.1.0-win64.zip
```

## 2. Code signing

```
:: certificate in the user's store, by SHA-1 thumbprint
cmake -B build -DBOSON_SIGN=ON -DBOSON_SIGN_THUMBPRINT=0123...ABCD
:: PFX + password in an environment variable (read at signing time, never cached)
set BOSON_SIGN_PFX_PASSWORD=...
cmake -B build -DBOSON_SIGN=ON -DBOSON_SIGN_PFX=C:/keys/codesign.pfx
:: Azure Trusted Signing (signtool /dlib)
cmake -B build -DBOSON_SIGN=ON -DBOSON_SIGN_DLIB=C:/tools/Azure.CodeSigning.Dlib.dll ^
      -DBOSON_SIGN_DLIB_METADATA=C:/ci/metadata.json -DBOSON_SIGN_TIMESTAMP_URL=http://timestamp.acs.microsoft.com
```

Every signature is `signtool sign /fd SHA256 /tr <BOSON_SIGN_TIMESTAMP_URL> /td SHA256` (default timestamp server
`http://timestamp.digicert.com`; empty disables timestamping). Exactly one certificate source must be configured or
the build fails; timestamp failures are retried (`BOSON_SIGN_RETRIES`, default 3) and the arguments (which may hold
the PFX password) are never echoed. Signed: targets given to `boson_sign_target()`, the MSIX of `<target>_msix`, and
`.msi` / `.exe` / `.msix` files produced by CPack (`CPACK_POST_BUILD_SCRIPTS`). `boson_sign_command(var FILES ...)`
returns the command for custom steps, and `cmake -DBOSON_SIGNTOOL=... -DBOSON_SIGN_FILES=... -P cmake/BosonSign.cmake`
works standalone in CI.

## 3. Auto-updater

### Release flow

```
boson_updater_sign keygen release-key.txt         :: once; prints "public_key: <base64>" to compile into the app
boson_updater_sign sign release-key.txt MyApp-1.4.0.zip 1.4.0 --url MyApp-1.4.0.zip --kind zip ^
    --notes "Faster startup" --rollout 25 --manifest manifest.json
boson_updater_sign verify <public_key> MyApp-1.4.0.zip 1.4.0 <signature>
```

The private key may also come from an environment variable (`env:VAR` instead of a path). Upload the package and
`manifest.json` over HTTPS. Manifest:

```json
{
  "version": "1.4.0",
  "url": "MyApp-1.4.0.zip",
  "sha256": "<64 hex>", "signature": "<base64 ECDSA P-256 r||s>", "size": 1234567,
  "kind": "zip", "installer_args": "",
  "notes": "Faster startup", "mandatory": false, "rollout": 25,
  "minimum_version": "1.0.0", "pub_date": "2026-11-26T00:00:00Z",
  "packages": { "msi": { "url": "https://cdn.example.com/MyApp-1.4.0.msi", "sha256": "...", "signature": "..." } }
}
```

- `version`: SemVer 2.0 (optional leading `v`, 1-4 numeric parts, pre-release precedence, build metadata ignored).
- `url`: absolute or relative to the manifest URL. `kind`: `msi` / `zip` / `exe` (default: URL extension).
- `packages`: per install kind; used when `UpdaterConfig::install_kind` has an entry, else the top-level package.
- `rollout`: staged rollout percentage; bucket = `SHA-256(install_id + "\n" + version) mod 10000`.
  `default_install_id()` hashes the machine GUID and user SID (nothing is written). Mandatory updates (`mandatory`,
  or the install is older than `minimum_version`) ignore the rollout.
- The signature covers `"boson-update-v1\n<version>\n<sha256 lower hex>\n"` (`update_signing_payload()`), so a
  validly signed old package cannot be replayed as a newer version.

### App side

```cpp
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    if (const auto code = boson::shipping::Updater::run_helper_if_requested()) {
        return *code;   // this process is the install helper
    }
    // ... window, marshal, runner, environment ...
    boson::shipping::Updater updater({
        .manifest_url = "https://updates.example.com/myapp/stable/manifest.json",
        .current_version = "1.3.2",
        .public_key = "JCXOFHXo29tG...",                    // from keygen
        .app_id = L"MyApp",
        .install_kind = boson::shipping::PackageKind::Zip,  // Msi for MSI installs
    });
    if (const auto last = updater.take_last_result()) { /* "updated to X", or last->error */ }

    updater.check_async(runner, marshal, [&](boson::shipping::CheckResult r) {          // UI thread
        if (!r.ok || !r.info.available) return;
        updater.download_async(runner, marshal, r.info,
            [](const boson::shipping::DownloadProgress& p) { /* pool thread */ },
            [&](boson::shipping::DownloadUpdateResult d) {                              // UI thread
                if (d.ok && updater.schedule_install(d.update)) boson::app::quit({environment});
            });
    });
    return boson::core::Window::run_message_loop();
}
```

- `check()` fetches the manifest with WinHTTP (TLS 1.2/1.3, HTTPS required, HTTPS-to-HTTP redirects refused;
  `HttpOptions::allow_insecure_loopback` and `allow_file_urls` are opt-ins for local tests and update shares).
- `download()` writes `<work_dir>/downloads/<version>-<name>.partial` and resumes an interrupted transfer with
  `Range` + `If-Range` (strong ETag or Last-Modified), so a file that changed on the server is fetched whole. Progress
  is reported with bytes, total and speed. Then the size, SHA-256 (CNG), the ECDSA P-256 signature
  (`BCryptVerifySignature`) and — if `authenticode_subject` is set and the package is an MSI/EXE — its Authenticode
  signer are checked. An empty `public_key` fails verification: unsigned updates are never installed. ZIPs are
  extracted with System32 `tar.exe` into `<work_dir>/staged/<version>`.
- `schedule_install()` re-verifies the package, writes a job file and starts a copy of the executable (or
  `helper_executable`) as `--boson-update-helper=<job>`, breaking away from kill-on-close job objects. The helper waits
  for the app (`InstallOptions::wait_pid`, default this process) to exit, then:
  - **MSI**: `msiexec /i <pkg> /qn /norestart /l*v <work_dir>/logs/msiexec-<ver>.log` (set `msi_ui = L"/passive"` for
    per-machine packages that need UAC). Windows Installer rolls back failed installs itself; exit 0 / 3010 / 1641 is
    success.
  - **ZIP**: each staged file replaces its counterpart in `install_dir`; originals are first renamed into
    `<install_dir>/.boson-update-backup` (same volume, so loaded DLLs and the old exe can move), locked files are
    retried for 20 s, and any failure puts every original back (`apply_portable_update()`).
  - **EXE**: runs the installer with `installer_args`; exit 0 is success.
  It writes `<work_dir>/last-result.json` (`take_last_result()`), logs to `<work_dir>/logs/update-helper.log` and
  relaunches the app with `--boson-updated=<ver>` or `--boson-update-failed=<ver>`. `cancel_scheduled_install()` kills
  a waiting helper; `wait_for_scheduled_install()` waits for it.
- MSIX-packaged apps (`is_packaged_process()`) should update through App Installer or the Store instead.

### JS: `UpdaterController` (controller name `Updater`)

| Method | Params | Result |
|---|---|---|
| `Updater.check` | – | `UpdateCheckResponse` |
| `Updater.download` | – | `UpdateStartedResponse`, then events `updater:progress` (`UpdateProgressEvent`) and `updater:downloaded` / `updater:error` (`UpdateResultEvent`) |
| `Updater.cancel` | – | – |
| `Updater.install` | `UpdateInstallRequest {relaunch, quit_now}` | `UpdateInstallResponse` |
| `Updater.status` | – | `UpdateStatusResponse` |
| `Updater.last_result` | – | `UpdateLastResultResponse` |

```cpp
boson::ipc::UpdaterController updater_controller(updater, bridge, runner,
    [&]() { marshal.post_task([&]() { boson::app::quit({environment}); }); });
```
```ts
import { updater, log } from './updater';
const info = await updater.check();
if (info.available) {
  await updater.download((p) => console.log(Math.round(p.fraction * 100) + '%'));
  await updater.install({ relaunch: true });
}
log('info', 'settings opened');
```

## 4. WebView2 runtime bootstrapper

```cpp
boson::shipping::RuntimeBootstrapOptions opts;
opts.consent = boson::shipping::message_box_consent(nullptr, L"My App");
const auto runtime = boson::shipping::ensure_webview2_runtime(opts);   // before creating windows
if (!runtime.ok()) return 1;
```

`installed_webview2_version()` wraps `GetAvailableCoreWebView2BrowserVersionString`. When no runtime is found,
`ensure_webview2_runtime()` asks for consent, downloads Microsoft's Evergreen bootstrapper
(`https://go.microsoft.com/fwlink/p/?LinkId=2124703`) with WinHTTP, requires a valid Authenticode signature from
"Microsoft Corporation", runs it with `/silent /install`, waits (10 min max) and checks again. Statuses:
`AlreadyInstalled`, `Installed`, `ConsentDeclined`, `DownloadFailed`, `VerificationFailed`, `InstallerFailed`,
`Cancelled`. `ensure_webview2_runtime_async(runner, marshal, opts, on_done)` runs on the pool and asks for consent on
the UI thread. It lives in the app library, so it works for ZIP, MSI and MSIX installs alike.

## 5. Crash reporting

```cpp
boson::shipping::CrashReporter crashes({.app_name = L"My App", .app_version = "1.4.0"});
crashes.install();                     // early in wWinMain
crashes.attach_webview(webview);       // ProcessFailed -> log (kind, reason, exit code, description)
runner.spawn([&] { crashes.process_pending_reports([](const boson::shipping::CrashReport& r) { return my_upload(r); }); });
```

- `install()` creates a dedicated writer thread. The unhandled-exception filter (and, with `catch_crt_errors`,
  `std::terminate`, pure-virtual calls, CRT invalid-parameter errors and `abort()`) only stores the exception pointers
  and wakes that thread, which calls `MiniDumpWriteDump` on a healthy stack — this covers stack overflows too. The
  dump (`<dump_dir>/<app>-<UTC time>-<pid>.dmp`, default dir `%LOCALAPPDATA%\<app>\crashes`) gets a `.json` sidecar
  (reason, exception code, pid, thread, app version). The process then exits with the exception code (no WER dialog).
  Everything the crash path touches is allocated in `install()`.
- `pending_reports()` / `process_pending_reports(upload)`: Boson never sends anything; the callback is your code, and
  reports it accepts are deleted. Only the newest `max_dumps` (10) are kept. `write_dump_now()` takes a live dump.
- `attach_webview()` keeps any `on_process_failed` handler that was set before and logs `describe(event)`, e.g.
  `GPU process exited (reason: crashed, exit code 0xC0000005, GPU)`, to the reporter's logger
  (`<dump_dir>/crash.log` unless you pass one).
- This is in-process with a dedicated thread, not a watchdog process: a crash that corrupts the heap or holds the
  loader lock can still lose the dump.

## 6. Logging

```cpp
auto logger = std::make_shared<boson::shipping::Logger>(boson::shipping::LoggerConfig{
    .directory = L"C:/Users/me/AppData/Local/MyApp/logs", .max_file_bytes = 5 << 20, .max_files = 5});
boson::shipping::Logger::set_default(logger);   // used by the updater / bootstrapper when they get no logger
logger->info("startup", "version {}", "1.4.0");
```

Lines (`2026-11-26T07:00:00.123Z INFO  [category] message (tid N)`, one record per line) are formatted on the caller's
thread and written by one writer thread; a full queue drops lines (`dropped_lines()`) rather than blocking the UI
thread. Files rotate at `max_file_bytes` to `boson.1.log` ... `boson.<max_files-1>.log`. `flush()` waits until
everything queued is written. `LogController` (name `Log`) exposes `Log.write({level, category, message})` to JS;
categories get a `web:` prefix.

## Notes for integration

- Controllers to register in tsgen: `boson::ipc::UpdaterController` (`boson/ipc/controllers/updater_controller.hpp`)
  and `boson::ipc::LogController` (`boson/ipc/controllers/log_controller.hpp`). Events: `updater:progress` ->
  `UpdateProgressEvent`, `updater:downloaded` / `updater:error` -> `UpdateResultEvent`.
- glaze 9.1 on MSVC silently left a struct member named `exception_code` at 0 when reading JSON; the crash sidecar
  therefore uses the key `"code"`.

## Verification

Tested on this machine (Windows 11 Pro 26300, MSVC Debug, /W4, zero warnings):

- `boson_shipping_tests`: **261/261 checks passed** — SemVer parsing and the SemVer 2.0 precedence chain; SHA-256
  vectors, incremental and file hashing; strict hex/base64; ECDSA P-256 keygen/sign/verify with tampered message,
  signature, wrong key and truncated inputs; manifest parsing/validation/relative URLs; rollout determinism and
  distribution; the logger (format, filtering, 4 writer threads, rotation limits); WinHTTP GET and HTTPS enforcement;
  a download cut off by a connection reset from an in-process loopback server and then resumed with 206; an ETag change
  forcing a full download; size pinning, cancellation and `file://`; the Updater end to end (check, mandatory /
  rollout / up-to-date, download + verify, ZIP extraction, rejection of replayed-version, flipped-signature,
  wrong-key, unsigned and swapped packages); the install helper as a real child process waiting for another PID and
  swapping files, the installer-EXE path, `cancel_scheduled_install()`; portable rollback with a locked file;
  minidumps from crashing child processes (access violation, stack overflow, `std::terminate`: exit code, `MDMP`
  header, sidecar, upload hook); manual dumps, pruning, ProcessFailed chaining/logging; WebView2 runtime detection,
  declined consent, refused plain-HTTP bootstrapper URL; Authenticode on `msedgewebview2.exe` (trusted, Microsoft
  Corporation) and on unsigned files; the `Log` and `Updater` controllers through the Router, including an
  asynchronous download.
- `boson_signing_test` (ctest, PowerShell): **13/13 checks passed** — creates a self-signed code-signing certificate
  in `Cert:\CurrentUser\My`, signs by thumbprint and by PFX + password env var through `BosonSign.cmake`, checks that
  misconfiguration fails, packs an MSIX with makeappx and signs it, then deletes the private key and removes the
  certificate from every CurrentUser store (verified).
- `boson_demo_msix` produced an MSIX ("Package creation succeeded"); `cpack -G ZIP` produced the ZIP (exe + MSVC
  runtime DLLs). `boson_updater_sign` keygen / sign / verify was run by hand (valid signature accepted, other version
  rejected).
- `boson_tests` ("All Boson tests passed.") and `boson_integration_tests` (108/108) still pass.

Not verified:

- **MSI**: WiX is not installed, so `cpack -G WIX` and the generated WiX fragment / patch (shortcut AUMID, protocol
  keys, `RemoveFolderEx`, per-user scope, v3 and v4 syntax) were never compiled, and the helper's msiexec branch never
  ran against a real MSI.
- Installing the MSIX (needs a trusted certificate) and its protocol extension.
- Timestamped signing and Azure Trusted Signing (no network timestamping in tests, no Azure account); the `/dlib`
  arguments follow Microsoft's documentation.
- Real HTTPS update servers (tests use plain HTTP on 127.0.0.1 with `allow_insecure_loopback`) and proxies.
- The WebView2 bootstrapper download and install (the runtime is installed here, and running Microsoft's installer
  in a test would change the machine), including whether a non-elevated `/silent /install` always installs per-user.
- The relaunch after an update (tests use `relaunch = false`) and an end-to-end update of a running Boson window.
- Crash dumps under a debugger (the filter is not called then) and crashes that corrupt the heap or loader lock.
