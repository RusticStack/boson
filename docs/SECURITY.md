# Security & storage

Everything in this document lives in `boson::security` (`include/boson/security/*`) plus three IPC controllers in
`include/boson/ipc/controllers/`. Windows 10/11 only.

| Piece | Header | What it does |
|---|---|---|
| Capabilities | `security/capabilities.hpp` | Tauri-v2-style per-WebView / per-origin allow-lists of `Controller.method` patterns, enforced in `Bridge` before dispatch |
| Origins | `security/origin.hpp` | Allocation-free origin parsing and patterns (`https://*.example.com`, `http://localhost:*`, `mailto:*`) |
| Web security | `security/web_security.hpp` | CSP + hardening headers on `boson://app` responses, navigation allow-list, new-window policy |
| Safe storage | `security/safe_storage.hpp` | DPAPI encrypt / decrypt (Electron `safeStorage`) |
| Credentials | `security/credential_store.hpp` | Windows Credential Manager, one target prefix per app |
| Scoped FS | `security/fs_scope.hpp`, `security/scoped_fs.hpp` | File access confined to granted roots; traversal / link / UNC / device protection; directory watching |
| Controllers | `ipc/controllers/{fs,safe_storage,credential}_controller.hpp` | JS `Fs`, `SafeStorage`, `Credential` |

Optional TypeScript wrappers: `frontend/src/security.ts` (`fs`, `safeStorage`, `credentials`, `isPermissionDenied`).

## 1. Capabilities (IPC permissions)

A `CapabilityPolicy` is compiled once (sorted exact names + prefix list per capability, origin patterns
pre-parsed) and shared immutably (`std::shared_ptr<const CapabilityPolicy>`). Per message the Bridge does an
allocation-free peek of `id`/`method` (glaze into `std::string_view`, params skipped) and a lookup; a rejected
call never reaches the router or the thread pool, and its params are never parsed.

Rules:
- Method patterns: `"Fs.read_text"`, `"Fs.*"`, `"Credential.get*"` (prefix), `"*"`.
- `origins` (optional) restricts a capability to page origins; empty = every origin the Bridge already accepts
  (`BridgeAttachOptions::allowed_origins` is still checked first).
- `deny` beats `allow` across all capabilities matching the origin.
- `default_deny` (default `true`): calls no capability allows are rejected. `false`: only `deny` rejects.
- Rejection reply: `{"id":N,"status":"error","error":{"code":-32003,"message":"permission denied: Fs.remove (denied by capability rule)"}}`.
  A request whose method cannot be read without allocation (malformed JSON, escaped method string) is rejected too.

```cpp
// C++
boson::security::CapabilityConfig caps;
caps.capabilities.push_back({.identifier = "main", .origins = {"boson://app"},
                             .allow = {"Boson.*", "Fs.*", "SafeStorage.*"}, .deny = {"Fs.remove"}});
auto policy = boson::security::CapabilityPolicy::make_shared(caps); // SecurityResult<shared_ptr<const ...>>

boson::ipc::BridgeAttachOptions opts;
opts.capabilities = *policy;
bridge.attach(webview, opts);
bridge.set_capabilities(webview.id(), other_policy); // swap at runtime (UI thread)
```

```cpp
// JSON (e.g. #embed-ed capabilities.json)
auto policy = boson::security::CapabilityPolicy::shared_from_json(R"({
  "default_deny": true,
  "capabilities": [
    {"identifier": "main", "origins": ["boson://app"], "allow": ["Boson.*", "Fs.*"], "deny": ["Fs.remove"]},
    {"identifier": "dev",  "origins": ["http://localhost:*"], "allow": ["*"]}
  ]
})");
```
Unknown keys and invalid patterns fail the load (fail closed). `CapabilityPolicy::check(method, uri)` /
`is_allowed()` can be used directly, and `to_json()` round-trips the config.

```ts
import { isPermissionDenied } from './security';
try { await boson.call('Fs.remove', { path }); } catch (e) { if (isPermissionDenied(e)) { /* ... */ } }
```

## 2. Content Security Policy and hardening headers

`apply_security_headers(WebViewConfig&, SecurityHeaders = {})` fills the new `WebViewConfig::asset_response_headers`
field; `ProtocolHandler` appends those lines to every `boson://app` response (200 and 404). Call it before
`WebViewHost::create` / `WebView::initialize`.

Defaults: `Content-Security-Policy` (below), `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`,
`Permissions-Policy: camera=(), microphone=(), geolocation=(), ...`, `Cross-Origin-Opener-Policy: same-origin`.

Default CSP (`default_content_security_policy()`):
```
default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline';
img-src 'self' data: blob:; font-src 'self' data:; connect-src 'self'; media-src 'self' blob:;
worker-src 'self' blob:; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'none'
```
Inline scripts and `eval` are blocked. IPC uses `chrome.webview.postMessage`, which CSP does not affect.

```cpp
boson::core::WebViewConfig cfg;
cfg.dev_url = opts.dev_url;
boson::security::SecurityHeaders headers;
headers.extra_connect_src = {"https://api.example.com"};
// headers.content_security_policy = "...";   // full custom policy
// headers.report_only = true;                // Content-Security-Policy-Report-Only
// headers.csp_enabled = false;               // off switch (other headers stay)
boson::security::apply_security_headers(cfg, headers);  // false if a value contains CR/LF
host.create(std::move(cfg), callbacks);
```
**dev_url:** when set, the dev server origin and its WebSocket (`ws://host:port`, HMR) are added to the source
lists. The header is only attached to `boson://app` responses; a page served by the dev server gets the dev
server's own headers (configure Vite `server.headers` if you want CSP in development).

### Navigation allow-list
```cpp
boson::security::NavigationPolicy nav;               // allowed_origins empty = boson://app + dev_url origin
nav.external_origins = {"https://*.github.com"};     // cancelled in the WebView, opened in the default browser
nav.otherwise = boson::security::NavigationAction::Block;
nav.on_rejected = [](std::string_view uri, auto) { /* log */ };
boson::security::install_navigation_policy(webview, std::move(nav));
```
Hooks `events().on_navigation_starting`; an existing handler is chained and only sees allowed navigations.
External opening uses `ShellExecuteExW` (`SEE_MASK_ASYNCOK`, UI thread does not wait) and is restricted to
`external_schemes` (default http/https/mailto); `file:`, `javascript:` etc. are only ever blocked.
`CompiledNavigationPolicy::decide(uri)` is the side-effect-free decision function.

### New windows
```cpp
boson::security::NewWindowPolicy popups;                  // default: http(s) -> system browser, rest blocked
popups.allowed_origins = {"boson://app"};
popups.allowed_action = boson::security::NewWindowAction::NavigateSelf; // or AllowDefault (chains)
boson::security::install_new_window_policy(webview, std::move(popups));
```

## 3. Safe storage (DPAPI)
```cpp
boson::security::SafeStorage storage{"optional-app-entropy"};        // CurrentUser scope by default
auto cipher = storage.encrypt_string("token");                         // SecurityResult<vector<byte>>
auto plain  = storage.decrypt_string(*cipher);                         // SecurityResult<string>
bool ok = boson::security::SafeStorage::is_available();
```
`CryptProtectData` / `CryptUnprotectData` with `CRYPTPROTECT_UI_FORBIDDEN`; `ProtectionScope::LocalMachine` adds
`CRYPTPROTECT_LOCAL_MACHINE`. Entropy must match to decrypt. Decrypted buffers are wiped with `SecureZeroMemory`.
JS (`SafeStorage`): `is_available`, `encrypt_string {text} -> {data}`, `decrypt_string {data} -> {text}`,
`encrypt_bytes {data} -> {data}`, `decrypt_bytes {data} -> {data}` (all `data` are base64).

## 4. Credential store
```cpp
boson::security::CredentialStore creds{"com.example.myapp"};  // targets "com.example.myapp/<key>"
creds.set("github", "ghp_...", "alice");
auto entry = creds.get("github");          // SecurityResult<optional<StoredCredential>>
auto keys  = creds.list();                 // CredEnumerateW("com.example.myapp/*"), exact-prefix filtered
creds.remove("github");                    // SecurityResult<bool>: false = did not exist
```
`CRED_TYPE_GENERIC`, `CRED_PERSIST_LOCAL_MACHINE`, secrets up to 2560 bytes, keys 1-200 chars without `*` or
control characters. JS (`Credential`): `set {key,secret,username}`, `get {key} -> {found,username,secret}`,
`remove {key} -> {value}`, `list -> {keys}`. This hands secrets to JS: gate it with a capability.

## 5. Scoped file system
```cpp
auto scope = std::make_shared<boson::security::FsScope>();
scope->allow_base_dir(boson::security::FsBaseDir::AppData, "MyApp");          // %APPDATA%\MyApp (created)
scope->allow_base_dir(boson::security::FsBaseDir::Documents, {}, /*read_only*/ true);
scope->allow("D:\\Projects", false, /*recursive*/ true, "projects");
auto fs = std::make_shared<boson::security::ScopedFs>(scope);              // max read 256 MiB by default

boson::ipc::FsController fs_controller(fs, {.dialog_owner = &window, .marshal = &marshal});
boson::ipc::Router<boson::ipc::SystemController, boson::ipc::FsController> router(system_controller, fs_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
connect_fs_events(fs_controller, bridge);     // watch() batches -> "fs:changed" event
```
Path checks (`FsScope::resolve`), all fail-closed:
1. Rejected lexically: UNC (`\\\\server`), Win32/NT namespaces (`\\\\?\\`, `\\\\.\\`, `\\??\\`), drive-relative (`C:foo`), rooted
   without drive, alternate data streams (`a.txt:s`, `::$DATA`), reserved device names (`CON`, `NUL.txt`,
   `COM1`, `LPT¹`, `CONIN$`...), trailing dot/space, wildcards, control characters, NUL, non-UTF-8, `..` above the drive.
2. `.`/`..` collapsed; relative paths are joined to the first directory root.
3. The deepest existing ancestor is opened and `GetFinalPathNameByHandleW` gives its real path: symlinks and
   junctions are followed before the containment test (component-wise, case-insensitive; `C:\\root-x` is not in
   `C:\\root`). Roots are canonicalised the same way.
4. read/write/append re-check the opened handle's final path (closes the resolve-then-open window).
5. remove/rename act on the link itself (parent resolved, leaf appended) and never follow it; scope roots cannot be
   removed or renamed; read-only scopes refuse every write.

Writes are atomic (temp file + `MoveFileExW(REPLACE_EXISTING)`). Watching uses overlapped
`ReadDirectoryChangesW` on the Win32 thread pool (`CreateThreadpoolIo`): no dedicated thread, max 64 watches,
events `fs:changed` with `{id, changes:[{kind, path}], overflow, closed}`.

JS (`Fs`): `read_text`, `read_bytes` (base64), `write_text`, `write_bytes`, `append`, `exists`, `stat`,
`read_dir`, `mkdir`, `remove`, `rename`, `copy`, `watch`, `unwatch`, `scopes`, `grant_from_dialog` (native
picker on the UI thread; chosen folders/files are added to the scope).

```ts
import { fs } from './security';
const text = await fs.readText('C:\\Users\\me\\AppData\\Roaming\\MyApp\\settings.json');
const stop = await fs.watch(dir, (batch) => console.log(batch.changes));
```

## Verification

`boson_security_tests` (tests/test_security.cpp), run on Windows 11 (build 26300) with the installed WebView2
runtime: **315/315 checks passed**. `boson_tests` still prints "All Boson tests passed." and
`boson_integration_tests` passes 108/108 (the Bridge, ProtocolHandler and WebViewConfig were touched).

Tested:
- Origin parsing/patterns (userinfo trick, IPv6, sub-domain, port wildcard, scheme-only, invalid patterns).
- Capability matching: exact/wildcard/prefix, deny-wins, default-deny on/off, per-origin, malformed methods,
  JSON loading with unknown keys/invalid patterns rejected, `to_json` round trip, header peek (params before
  method, escaped method rejected).
- Real WebView: CSP/nosniff/Referrer-Policy/Permissions-Policy present on fetched `boson://app` responses (200
  and 404), inline script actually blocked, `connect-src` blocks other origins; Bridge returns -32003 for denied
  and unknown methods and executes allowed ones; `set_capabilities` swap; navigation policy cancels and routes to
  `open_external` (captured, no browser launched); chained handler only sees allowed navigations; `window.open`
  to https goes external, other schemes blocked.
- DPAPI round trip (text, binary, empty, LocalMachine), entropy mismatch and tampering fail; base64 vectors.
- Credential Manager set/overwrite/get/list/delete with a unique per-run prefix; everything removed at the end.
- Scoped FS: all operations, `..` traversal (absolute and relative), sibling-prefix directory, UNC,
  `\\?\`, `\\.\`, `GLOBALROOT`, device names, ADS, non-UTF-8, read limit, read-only scope, non-recursive
  and single-file scopes, root removal; a real junction (`mklink /J`) pointing outside: reads, listings, writes,
  mkdir and copies through it denied, removing the junction leaves the target intact; directory watch delivers
  events and stops after unwatch; the controllers through `Router::dispatch_sync`.

Not verified:
- File symlinks: creating them needs Developer Mode or the symlink privilege, which this machine lacks, so that
  block is skipped (junctions, the same reparse mechanism for directories, are tested).
- Real `ShellExecuteExW` launching a browser (tests inject `open_external` so nothing is opened on the machine).
- `Fs.grant_from_dialog` (needs a user to pick in the dialog); only its "no owner configured" error is tested.
- Network (UNC) scopes, e.g. a redirected Documents folder: the code compares final `\\?\UNC\...` paths but no
  share was available.
- Watcher behaviour when the watched directory itself is deleted (reported as a `closed` batch) and buffer overflow.
- The capability check is allocation-free by construction (string_view peek, no copies); it was not measured.

Known limits: a hard link inside a scope to a file outside it is indistinguishable from a normal file (Windows has
no "final path" for hard links). Checks are TOCTOU-hardened for read/write/append (handle re-verification) but
directory-level operations (mkdir, remove, rename, copy) resolve then act.
