# WebView features

Print/PDF, page capture, find-in-page, cookies, profiles, proxy, and authentication/certificate events for
`boson::core::WebView`. Everything is built on the WebView2 SDK 1.0.4370.26 and lives in
`include/boson/webview/*.hpp` (namespace `boson::webview`). The JS controller is in
`include/boson/ipc/controllers/webview_controller.hpp`.

Threading: every `boson::webview` function must be called on the UI thread, and its callback runs on the UI
thread. The async operations also come as `exec::CallbackSender<T>` overloads for `std::execution`
pipelines. Never `sync_wait` one of these on the UI thread. A function returns `false` and does not invoke
the callback when the operation could not start: the WebView is not ready, the runtime is too old, or the
arguments are invalid. The reason is logged through `hr_check`.

## Print and PDF (`boson/webview/print.hpp`)

```cpp
using namespace boson::webview;
PrintSettings s{
    .orientation = PrintOrientation::Landscape,
    .margin_top = 0.4,               // inches
    .print_backgrounds = true,
    .header_and_footer = true,
    .header_title = "Report",
    .page_ranges = "1-3, 5",
    .color_mode = PrintColorMode::Grayscale,
};
print_to_pdf(view, L"C:\\out\\report.pdf", s, [](HRESULT hr, bool ok) { /* ... */ });
print_to_pdf_stream(view, s, [](HRESULT hr, std::vector<std::byte> pdf) { /* ... */ });
print(view, PrintSettings{.copies = 2, .duplex = PrintDuplex::TwoSidedLongEdge, .printer_name = "Office"},
      [](HRESULT hr, PrintStatus st) { /* Succeeded / PrinterUnavailable / OtherError */ });
show_print_ui(view, PrintDialogKind::System);   // or PrintDialogKind::Browser (Edge preview)
```

Every field of `PrintSettings` is optional. A field you leave unset keeps the runtime default.
`validate_print_settings()` checks these ranges before anything reaches COM:

| Field | Allowed values |
| --- | --- |
| `scale_factor` | 0.1 to 2.0 |
| page size | greater than 0 |
| margins | 0 or more |
| `pages_per_side` | 1, 2, 4, 6, 9 or 16 |
| `copies` | 1 to 999 |
| `page_ranges` | comma-separated `N`, `N-M`, `N-` and `-M` items |

`copies`, `collation`, `duplex` and `printer_name` affect only the silent `print()`.

## Capture (`boson/webview/capture.hpp`)

```cpp
capture_preview(view, ImageFormat::Png, [](HRESULT hr, std::vector<std::byte> png) { /* ... */ });
capture_preview_to_file(view, ImageFormat::Jpeg, L"C:\\out\\shot.jpg", [](HRESULT hr) { /* ... */ });
```

These functions capture the visible viewport at physical-pixel size, using `ICoreWebView2::CapturePreview`.
The header also has `encode_base64()` and `has_image_signature()`.

## Find in page (`boson/webview/find.hpp`)

```cpp
FindSession find(view);                       // after the WebView is ready
find.set_state_callback([](const FindState& s) { /* s.match_count, s.active_match_index (1-based) */ });
find.start({.term = "apple", .case_sensitive = false, .whole_word = true, .suppress_default_dialog = true});
find.next(); find.previous(); find.stop();
```

- Find needs `ICoreWebView2_28`, so `is_supported()` is false on runtimes that don't have it.
- Set `suppress_default_dialog = false` to show the Edge find bar.
- A session is bound to the current `ICoreWebView2`. Create a new session after `Environment::recreate()`.

## Cookies (`boson/webview/cookies.hpp`)

```cpp
set_cookie(view, Cookie{.name = "sid", .value = "x", .domain = "example.com", .path = "/",
                        .expires = unix_seconds, .http_only = true, .secure = true,
                        .same_site = CookieSameSite::Strict});
get_cookies(view, "https://example.com/app", [](HRESULT, std::vector<Cookie> list) { /* ... */ });
delete_cookies(view, "sid", "https://example.com/");            // by name + uri
delete_cookies(view, "sid", "example.com", "/");                // by name + domain + path
delete_all_cookies(view);
```

- `expires == nullopt` means a session cookie.
- `validate_cookie()` rejects invalid names or values, a non-host domain, a path that doesn't start with
  `/`, and `SameSite=None` without `secure`.
- The cookie manager belongs to the WebView's profile.

## Profiles (`boson/webview/profile.hpp` and `WebViewConfig`)

Three new `WebViewConfig` fields select the profile when the controller is created:

```cpp
boson::core::WebViewConfig cfg;
cfg.profile_name = L"Work";        // own cookies, storage, cache; persisted under <udf>/EBWebView/WV2Profile_work
cfg.in_private = true;             // nothing persisted
cfg.script_locale = L"de-DE";      // JS Intl / Date locale (ICoreWebView2ControllerOptions2)
```

When any of these is set, the controller is created with
`ICoreWebView2Environment10::CreateCoreWebView2[Composition]ControllerWithOptions`. Otherwise the original
code path runs unchanged. Profile names must pass `is_valid_profile_name()`: 1 to 64 characters, using
letters, digits and ` #@$()+-_~.`, and not ending in a space or a period.

```cpp
auto info = profile_info(view);    // name, in_private, path, default_download_folder, color_scheme, tracking_prevention
set_preferred_color_scheme(view, ColorScheme::Dark);
set_default_download_folder(view, L"D:\\Downloads");
set_tracking_prevention_level(view, TrackingPreventionLevel::Strict);
clear_browsing_data(view, BrowsingDataKind::Cookies | BrowsingDataKind::DiskCache,
                    /*start*/ unix_now - 3600, /*end*/ std::nullopt, [](HRESULT) {});
clear_browsing_data(view, BrowsingDataKind::None, {}, {}, cb);   // None = ClearBrowsingDataAll
```

Profile setters apply to every WebView that shares the profile.

## Proxy (`boson/webview/proxy.hpp`)

WebView2 1.0.4370.26 has no runtime or per-profile proxy API. A proxy is a browser-process (Environment)
setting made with Chromium switches:

```cpp
boson::core::EnvironmentConfig env;
boson::webview::apply_proxy(env, {.mode = ProxyMode::Fixed, .server = L"http=proxy:8080;https=proxy:8443",
                                  .bypass_list = {L"<local>", L"*.corp.example"}});
// or {.mode = ProxyMode::PacScript, .pac_url = L"http://wpad/proxy.pac"}, ProxyMode::AutoDetect, ProxyMode::Direct
```

The modes map to these switches:

| Mode | Switches |
| --- | --- |
| `ProxyMode::Fixed` | `--proxy-server` and optionally `--proxy-bypass-list` |
| `ProxyMode::PacScript` | `--proxy-pac-url` |
| `ProxyMode::AutoDetect` | `--proxy-auto-detect` |
| `ProxyMode::Direct` | `--no-proxy-server` |
| `ProxyMode::System` | none: the runtime follows the system settings |

`apply_proxy` first removes any proxy switches already in `additional_browser_arguments`, so you can call
it repeatedly. To change the proxy while the app runs, call `Environment::recreate(new_config)`.

To give WebViews different proxies, use separate Environments, each with its own user data folder.
Profiles can't have separate proxies.

## Spellcheck and language

The 1.0.4370.26 SDK has no spellcheck API. `ICoreWebView2Settings` has no spellcheck toggle, and the SDK has
no dictionary or language list. What you can control:

- The UI and Accept-Language language: `EnvironmentConfig::language` (for example `L"en-US"`), applied
  through `ICoreWebView2EnvironmentOptions::put_Language`.
- The JavaScript locale per WebView: `WebViewConfig::script_locale`.
- Spellchecking for a field: the standard HTML `spellcheck="true|false"` attribute.

The runtime decides which spellchecker and dictionaries it uses. Boson does not configure or test that.

## Authentication and certificate events (`boson/webview/security_events.hpp`)

`WebViewEventHandlers` has three new callbacks. They are registered with the WebView's other handlers, so
they survive `Environment::recreate()`. If a callback is unset, the runtime shows its default UI.

```cpp
view.events().on_basic_auth_requested = [](boson::webview::BasicAuthRequest& r) {
    r.provide({.username = "user", .password = "pass"});         // or r.cancel(); or auto done = r.defer();
};
view.events().on_client_certificate_requested = [](boson::webview::ClientCertificateRequest& r) {
    // r.host(), r.port(), r.certificates() (subject, issuer, validity, PEM, kind)
    r.respond(ClientCertificateAction::Select, 0);               // Default / NoCertificate / Cancel
};
view.events().on_server_certificate_error = [](boson::webview::ServerCertificateError& e) {
    // e.request_uri(), e.error_status(), e.certificate()
    e.set_action(ServerCertificateErrorAction::Cancel);          // AlwaysAllow / Default
};
boson::webview::clear_server_certificate_error_actions(view);    // forget AlwaysAllow decisions
```

Each request also has a `defer()` method. It returns a completion function that you call once, later, on
the UI thread.

## JS controller "WebView" (`boson::ipc::WebViewController`)

Construct one controller per WebView and router, like `SystemController`:

```cpp
boson::ipc::WebViewController web(view, marshal);
boson::ipc::Router<boson::ipc::SystemController, boson::ipc::WebViewController> router(system, web);
boson::ipc::Bridge bridge(view, runner, marshal, router);
web.on_find_state = [&](const boson::ipc::FindStatus& s) {
    bridge.emit_to(view.id(), boson::ipc::WebViewController::kFindEvent, s);   // "webview:find"
};
```

The controller has these methods:

- Print: `show_print_ui`, `print`, `print_to_pdf`, `print_to_pdf_data` (returns base64)
- Capture: `capture` (base64, or written to `path`)
- Find: `find_start`, `find_next`, `find_previous`, `find_stop`, `find_status`
- Cookies: `get_cookies`, `set_cookie`, `delete_cookies`, `delete_all_cookies`
- Profile: `clear_data`, `get_profile`, `set_color_scheme`

How the calls run:

- Methods run on the router's pool thread.
- Each method marshals its COM call to the UI thread and waits there for the completion. The wait has a
  timeout, 60 s by default.
- Invalid arguments come back as JSON errors, for example an unknown `duplex` or an invalid cookie.

The types to register in tsgen are in `boson/ipc/controllers/webview_controller.hpp`. The typed frontend
helpers are in `frontend/src/webview.ts`:

```ts
import { webview } from './webview';
const shot = await webview.capture('png');            // shot.data is base64
await webview.findStart({ term: 'apple' });
const off = webview.onFind(s => console.log(s.match_count));
await webview.clearData(['cookies', 'disk_cache']);
```

## Verification

All results below were produced on this machine: Windows 11, WebView2 runtime 158.0.4370.33, SDK
1.0.4370.26.

`tests/test_webview_features.cpp` builds as `boson_webview_features_tests` and passed 257 of 257 checks. It
contains 92 headless unit checks and 165 checks against the real runtime. The runtime checks cover:

- **Print to PDF:** `print_to_pdf` wrote a file that starts with `%PDF`. The run used landscape, margins,
  a header and footer, a page range and grayscale. `print_to_pdf_stream` returned bytes that start with
  `%PDF`. Invalid settings were rejected before COM.
- **Silent print:** `print()` to a printer name that doesn't exist reported `PrinterUnavailable`. No real
  printer was used.
- **Capture:** the PNG and JPEG bytes have valid signatures. The PNG's IHDR size equals the WebView's
  bounds. Capture to a file also produced a valid PNG.
- **Find:** `apple` gave 5 matches case-insensitive, 3 case-sensitive and 4 whole-word. Next, previous
  and wrap-around moved the active index, state events fired, and stop worked.
- **Cookies:** set, get, update, a path mismatch, delete by URI, delete by domain and path, delete all, and
  attribute round-trips (HttpOnly, Secure, SameSite, expiry, session).
- **Profiles:**
  - A named profile, the default profile and an InPrivate WebView kept their cookies separate.
  - The reported profile names and InPrivate flags were correct. The named profile's path is
    `WV2Profile_<lower-cased name>`.
  - With `script_locale = de-DE`, `Intl` reported de-DE.
  - The color scheme changed `prefers-color-scheme`.
  - The tracking prevention level and download folder read back as set.
- **Clearing data:** clearing cookies (all time, and a time range), clearing an http origin's
  `localStorage`, and `ClearBrowsingDataAll` all worked. Clearing one profile left the other profile
  untouched. Observation: clearing `LocalStorage` did not remove `localStorage` of the custom `boson://app`
  origin in this runtime. That is why the test uses an http origin.
- **BasicAuthenticationRequested:** a loopback HTTP server answered with 401. The handler answered through
  `defer()` with credentials, and the server received the correct `Authorization` header.
- **Proxy:** an Environment built with `apply_proxy(Fixed, 127.0.0.1:<port>)` sent
  `http://boson-proxy.test/` through the loopback server.
- **WebView controller:** 12 RPCs went end to end through Router and Bridge from page JavaScript: capture
  (PNG and JPEG), find start and stop with `webview:find` events, set, get and clear cookies, PDF data and
  PDF file, get profile, and an invalid-argument error.

`boson_tests` and `boson_integration_tests` still pass. The latter covers the changed controller-creation
path.

The following are implemented but **not** verified on this machine:

- `show_print_ui`: it opens an interactive dialog.
- A successful silent `print()` to a real printer: it would use paper or a "save as" prompt.
- `ClientCertificateRequested` and `ServerCertificateErrorDetected`: they need a TLS server with a client
  certificate requirement or a bad certificate.
- `clear_server_certificate_error_actions`.
- `PacScript`, `AutoDetect` and `Direct` proxy modes: only their switch generation is unit-tested.
- Profiles with `HostingMode::Composition`: only the windowed path ran with options.
