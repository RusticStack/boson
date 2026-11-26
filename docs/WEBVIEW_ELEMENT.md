# <boson-webview>: embedded guest pages, guest hooks, CDP

`<boson-webview>` replaces Electron's `<webview>` tag. Each element owns a native WebView2 ("guest") stacked on top of the
host page and kept on the element's box. Every capability exists at three levels:

| Level | Where | What |
|---|---|---|
| C++ API | `include/boson/webview/guest.hpp`, `element.hpp`, `cdp.hpp`, `cdp_server.hpp` | `Guest`, `GuestManager`, `compute_element_placement`, `Cdp`, `CdpServer`, `CdpGuestBridge` |
| Controller | `include/boson/ipc/controllers/guest_controller.hpp` (JS name `Guest`), `WebView.cdp_send` / `cdp_subscribe` | JSON-RPC methods (glaze reflection) + `guest:*` events |
| JS | `frontend/src/webview_element.ts`, `compat/electron.ts` (`WebviewTag` type) | the custom element and its Electron-style API |

Boson itself has no Node dependency. Playwright is only used, optionally, to test the CDP endpoint.

## Setup (C++)

```cpp
// src/main.cpp (UI thread, after the host WebView is created)
#include <boson/boson.hpp>

boson::webview::GuestManager guests(host, {
    // <boson-webview preload="..."> is refused unless the app resolves the URL (never fetched from disk / network).
    .resolve_preload = [](std::string_view url) -> std::optional<std::string> {
        if (url == "boson://app/preload.js") return std::string(kPreloadJs);
        return std::nullopt;
    },
});
boson::ipc::GuestController guest_controller(guests, marshal);
boson::ipc::Router<boson::ipc::WebViewController, boson::ipc::GuestController> router(webview_controller, guest_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
connect_events(guest_controller, bridge); // guest:* events go to the page that owns the guest
bridge.attach(page);
// Optional: large captures / PDFs through shared buffers instead of base64.
guest_controller.set_binary_transport(&binary_transport);
```

Destroy order: `disconnect_events(guest_controller)` (or destroy the controller) before the bridge; the `GuestManager` before
the `WebViewHost`. Guests are never attached to the Bridge: a guest page cannot call any controller.

`examples/webview_element/main.cpp` (target `boson_webview_element`, built when bun is available) is a complete app: two panes
in different partitions in a draggable split, URL bar for the focused pane, event log, popups adopted into an overlay, and
`--cdp` to start the DevTools endpoint.

## The element (JS)

```ts
import './webview_element'; // registers <boson-webview>

const wv = document.createElement('boson-webview');
wv.setAttribute('src', 'https://example.com');
wv.setAttribute('partition', 'persist:docs');
wv.setAttribute('allowpopups', '');
wv.addEventListener('did-navigate', (e) => console.log(e.url, e.httpResponseCode));
wv.addEventListener('ipc-message', (e) => console.log(e.channel, e.args));
document.body.append(wv);
const title = await wv.executeJavaScript('document.title');
```

Attributes (Electron names): `src`, `partition`, `preload`, `allowpopups`, `useragent`, plus `zoom` (factor),
`popups` (`deny` | `guest` | `window`, overrides `allowpopups`), `permissions` / `basicauth` / `certificateerrors`
(`default` | `deny` | `allow` | `ask`), `reservedaccelerators` (`"Ctrl+W,Ctrl+T"`, kept inside the guest), `console`
(report `console-message`), `z` (stacking among guests), `guestinstance` (adopt an existing guest, e.g. a popup),
`nodevtools`, `nocontextmenu`, `noaccelerators`, `downloadui`. `src`, `zoom`, `useragent` and the policies can change at any time.

Partitions: `""` = the environment's default profile, `persist:name` = a named profile on disk, `name` = an in-memory
(InPrivate) named profile shared by every guest using the same name, `inprivate` = an anonymous in-memory profile.

Methods: `loadURL`, `goBack`, `goForward`, `reload`, `stop`, `executeJavaScript(code, userGesture?)`, `insertCSS` / `removeInsertedCSS`,
`findInPage` / `stopFindInPage`, `setZoomFactor`, `setAudioMuted`, `setUserAgent`, `capturePage(format?)` (base64 + `toDataURL()` /
`toBlob()`), `capturePageTo(channel)` (BinaryTransport), `print(options)`, `printToPDF(options)` (Uint8Array), `openDevTools`,
`getWebContentsId`, `send(channel, ...args)`, `getURL`, `getTitle`, `isLoading`, `canGoBack`, `canGoForward`, `isCrashed`,
`respondToPermission`, `respondToLogin`, `respondToCertificateError`, `downloadAction(id, 'cancel' | 'pause' | 'resume')`,
`cdp.send(method, params, sessionId?)`, `cdp.on(event, listener)`, `whenReady()`. Static `BosonWebViewElement.layoutStats()`.

Events (DOM events on the element; payload fields are on the event and in `event.detail`): `did-start-loading`, `did-navigate`
(`httpResponseCode`), `did-navigate-in-page`, `dom-ready`, `did-finish-load`, `did-fail-load` (`errorCode`, `errorDescription`,
`validatedURL`), `page-title-updated`, `page-favicon-updated`, `new-window` (`disposition`, `guestInstanceId`), `console-message`
(`level` 0-3, `message`, `line`, `sourceId`), `ipc-message` (`channel`, `args`), `crashed` + `render-process-gone`, `close`,
`media-started-playing` / `media-paused`, `download-started` / `download-updated` / `download-done`, `permission-request`, `login`,
`certificate-error`, `select-client-certificate`, `found-in-page` (`result`), `cdp-event`, `focus-out`, `guest-focus` / `guest-blur`,
`guest-ready`, `destroyed`.

Inside the guest, `window.bosonGuest` (top frame only, non-writable): `sendToHost(channel, ...args)` raises `ipc-message`,
`on(channel, (event, ...args) => ...)` / `once` / `off` receive `element.send(channel, ...args)`. Plain `chrome.webview.postMessage`
calls without the guest tag are ignored.

Listeners added to an element **before** the module defines `<boson-webview>` (an un-upgraded element) are not seen by the element,
so `console-message` is then only reported with the `console` attribute. The module calls `Guest.destroy_all` once per document,
so guests left over by a reloaded host page are destroyed.

## Layout model

- Measurement happens in the host page, application in C++. `ResizeObserver` (elements + root), `IntersectionObserver`, a
  `MutationObserver` (style / class / hidden / child list), capture-phase `scroll`, `resize`, `visibilitychange` and a DPR media query
  only mark elements dirty. One `requestAnimationFrame` flushes every dirty element and sends **one** `Guest.set_layout` call
  (fire-and-forget, posted to the UI thread). A 100 ms timer backs up rAF for documents that render no frames (occluded or hidden
  host window). CSS transitions / animations (`transitionrun` … `animationend`) keep the flush running every frame only while they run.
  Nothing runs while nothing changes. Unchanged placements are dropped in JS and again in C++.
- The clip is the intersection of the viewport and every overflow-clipping ancestor on the element's containing-block chain
  (absolutely / fixed positioned boxes escape ancestors that do not contain them; `contain: paint` clips). It is applied with
  `SetWindowRgn` on the guest's container window.
- CSS px × `devicePixelRatio` + the host WebView's origin; each edge is rounded separately, so adjacent elements neither overlap nor leave a
  gap (`compute_element_placement`).
- Hidden when `display: none` / `visibility: hidden` (`checkVisibility`), zero size, disconnected, or clipped away entirely.
  Minimizing the host window hides guests by itself (child windows).
- Stacking: guests of one page are ordered by `z`; they are always above the host page (native windows cannot be under HTML).
  Content that must overlap a guest (menus, dialogs) has to be placed outside it or the guest hidden meanwhile.
- **Composition hosting** (`HostingMode::Composition`) moves and hides guests but does not clip them.

## Focus and accelerators

- The element is focusable (`tabindex=0`). Focusing it (Tab, Shift+Tab, `focus()`) moves focus into the guest with
  `MoveFocus` (next / previous / programmatic). Tabbing out of the guest (`MoveFocusRequested`) raises `focus-out` and focuses the
  next / previous tabbable element of the host page. Guest `GotFocus` / `LostFocus` raise `guest-focus` / `guest-blur`.
- Keys pressed in the guest with Ctrl, Alt or Win, and F1-F24, are forwarded: the element dispatches a bubbling, cancelable `keydown`
  `KeyboardEvent` (DOM `key` / `code`), so host shortcuts written for the host page keep working. `reservedaccelerators` are
  swallowed (`Handled`) so the guest page never sees them.

## Guest hooks

| Hook | C++ (`GuestOptions`) | Event | Answer |
|---|---|---|---|
| Popups | `popups = Deny | Guest | Window` | `guest:new-window` (`disposition`, `new_guest_id`) | adopt with `guestinstance` |
| Downloads | `download_folder`, `hide_download_ui` | `guest:download-started/updated/done` (progress throttled to 100 ms) | `Guest.download` cancel / pause / resume |
| Permissions | `permissions` | `guest:permission-request` | `respond_permission` |
| Basic / proxy auth | `basic_auth` | `guest:login` | `respond_login` |
| Server certificate errors | `certificate_errors` (default `Deny`) | `guest:certificate-error` | `respond_certificate` |
| Client certificates | `client_certificates` | `guest:select-client-certificate` | `respond_client_certificate` |
| User agent | `user_agent` | — | `set_user_agent` |

"ask" requests are deferred (WebView2 deferrals) until answered; destroying the guest cancels them. A guest can own at most
`max_guests_per_owner` (64) guests including its popups.

The preload runs before page scripts in every document of the top frame, inside a function scope **in the page's main world**
(WebView2 has no isolated worlds for `AddScriptToExecuteOnDocumentCreated`); it is not a security boundary.

## Chrome DevTools Protocol

- C++: `webview::Cdp cdp(webview); cdp.call("Runtime.evaluate", R"({"expression":"1"})", [](bool ok, std::string json) {...});`
  (also a sender: `cdp.call(method, params)` → `CallbackSender<std::string>`), `subscribe(event, fn)`, `subscribe_domain("Network", fn)`.
  One event receiver per event name, created on first subscription and released with the last one. Flattened child sessions
  (OOPIFs, workers) go through `CallDevToolsProtocolMethodForSession`; events carry their session id.
- JS: `WebView.cdp_send({method, params, session_id})` / `WebView.cdp_subscribe({events, subscribe})` on the calling WebView (events via
  `WebViewController::on_cdp_event`, suggested name `cdp:event`), and `element.cdp.send` / `element.cdp.on` (`Guest.cdp_send`, `guest:cdp-event`) for guests.
- Remember `<Domain>.enable` before expecting events. All clients of one WebView share its DevTools session.

### Loopback endpoint (`CdpServer`)

```cpp
boson::webview::CdpServer cdp;              // 127.0.0.1, random port, 32 hex random token
cdp.start();
boson::webview::CdpGuestBridge cdp_guests(cdp, guests, {.owner_id = page.id()}); // guests become targets
// chromium.connectOverCDP(cdp.http_endpoint())   e.g. http://127.0.0.1:53121/9F3C.../
```

- Runs entirely on the UI thread: `WSAAsyncSelect` delivers socket readiness as window messages, no thread, no polling, no
  timers; nothing is subscribed until a client enables a domain, and everything is released when the last client leaves
  (`<Domain>.disable` for Fetch, Network, Debugger, DOM, …).
- Security: bound to 127.0.0.1 only; every HTTP and WebSocket path must start with the token (`/<token>/json/version`, …); the
  `Host` header must be a loopback name (DNS rebinding) and browser `Origin`s are refused unless listed in `allowed_origins`.
- HTTP: `/json/version`, `/json` and `/json/list`, `/json/new?url`, `/json/close/<id>`, `/json/activate/<id>`; WebSocket:
  `/devtools/browser/<id>` (browser endpoint, flattened sessions) and `/devtools/page/<targetId>`.
- Browser-level commands implemented by the server: `Target.setAutoAttach`, `setDiscoverTargets`, `getTargets`, `getTargetInfo`,
`attachToTarget` (flatten only), `detachFromTarget`, `createTarget`, `closeTarget`, `createBrowserContext` /
`getBrowserContexts` / `disposeBrowserContext`, `Browser.getVersion`, `getWindowForTarget`, `getBrowserCommandLine`,
`Storage.getCookies` / `setCookies` / `clearCookies`; other `Browser.*` commands are acknowledged with `{}`. Page-session commands are
  forwarded to WebView2 unchanged.
- Target ids are the pages' main frame ids (as in Chrome; Playwright relies on it). `Target.createTarget` calls the app's handler
  (`CdpGuestBridge` creates a guest; a `browserContextId` becomes an in-memory partition, so contexts are isolated).
- Limits: one automation client per target is safe (`Runtime.enable` from one client re-enables for all, the server sends
  `Runtime.disable` first so the client gets the execution-context events it expects). No `Tracing` / browser-wide
  `SystemInfo`; `Browser.setDownloadBehavior` is acknowledged but downloads follow the guest's options.

### Playwright

```js
const browser = await chromium.connectOverCDP(endpoint);   // playwright-core is enough (no browser download)
const page = browser.contexts()[0].pages().find((p) => p.url().includes('example.com'));
await page.goto('https://example.com/login'); await page.fill('#user', 'me'); await page.click('text=Sign in');
```

## Performance

Release build (`/O2`), WebView2 158.0.4370.33, Windows 11, DPR 1, measured by `boson_webview_element_tests`:

| Hot path | Cost |
|---|---|
| `compute_element_placement` | 20 ns |
| `ws::parse_frame`, 1 KiB masked frame | 0.41 µs (2.5 GB/s) |
| `GuestManager::apply_layout`, 8 guests, nothing changed | 0.11 µs / frame |
| … 8 guests move (scroll) | 3.7 ms / frame (≈ 0.46 ms per guest: `SetWindowPos` + `put_Bounds` + `NotifyParentWindowPositionChanged`) |
| … 8 guests move + region change | 6.0 ms / frame |
| … 8 guests resize | 5.2 ms / frame |
| JS layout flush (measure + diff, 1 element) | ≈ 0.1-0.2 ms (`layoutStats().lastFlushMs`) |
| Scroll → native move observed by the test | 1.6-2.6 ms (includes an ExecuteScript round trip) |
| `Cdp::call` Runtime.evaluate round trip | 184 µs |
| Runtime.evaluate through `CdpServer` (WebSocket, flattened session) | 151 µs |
| Idle cost | 0: no timers, no polling; the CDP server waits on window messages |

Moving native windows dominates: keep the number of simultaneously moving guests small (hide guests that are scrolled away;
hidden guests cost nothing per frame).

## Verification

Automated, `boson_webview_element_tests` (registered with `add_test`, label `integration`): 99 headless unit checks and, against
WebView2 158.0.4370.33, 321 checks in total in a Debug build (313 in Release) with Playwright enabled; 284 when Playwright is skipped. It
opens one non-activated window. `BOSON_WE_ONLY=element,partition,preload,ipc,popup,download,hooks,crash,cdp,wvc,perf,server` runs
selected sections.

Verified at runtime:
- Element end to end (host page + bundled `webview_element.ts` + `GuestController` over the Bridge): the native view follows the element's
  box; scroll inside an `overflow: auto` container moves it and clips it (window region checked, including top and bottom clips and
  region removal); resize; position-only moves (content inserted above, container moved); hide / show for `display: none`, ancestor
  `visibility: hidden` and scrolled out of view; `ipc-message`; `executeJavaScript` (awaits promises); `send` →
  `bosonGuest.on`; `console-message` with level; `loadURL`, `canGoBack`, `goBack`; `capturePage` (PNG); `printToPDF`;
  `getWebContentsId`; removing the element destroys the guest; unknown guest ids are rejected.
- Partitions isolate cookies (persistent named, in-memory named, default) and two guests with the same in-memory partition share them.
- Preload runs before page scripts, again after navigation, and never in the host page.
- Guest → host `ipc-message` with JSON args, host → guest `send`, untagged `postMessage` ignored.
- Popups: `window.open` with the guest policy creates an owned guest that loads the URL (`new-window` + `created`); deny policy reports
  `denied` and creates nothing.
- Downloads: started → completed with the right size and folder; a slow download is cancelled (`cancelled`, `user-canceled`).
- Permission prompt answered through the "ask" flow (Notification permission granted); basic auth answered through "ask";
  per-guest user agent; zoom; `insertCSS` / `removeInsertedCSS`; `findInPage` results; renderer crash (`Page.crash`) → `crashed`.
- CDP: commands, errors, event subscription / unsubscription (receiver count), relay to `guest:cdp-event`; `WebView.cdp_send` /
  `cdp_subscribe` / `on_cdp_event` on the host page.
- CDP endpoint with a raw WebSocket client: token required (404 without), `/json/version`, `/json/list`, RFC 6455 handshake,
  `Browser.getVersion`, `Target.getTargets`, `attachToTarget` (flatten) with `attachedToTarget`, session commands and events,
  `createTarget` → a guest that loads the URL, `closeTarget`, unknown-method error.
- Playwright (playwright-core 1.65.1 on Node 26, installed outside the repo, `BOSON_PLAYWRIGHT_DIR`; `tests/js/playwright_cdp.mjs`):
  `connectOverCDP`, `contexts()/pages()`, `page.title`, `page.evaluate`, `page.goto`, `locator.textContent`, `page.fill` + `page.click`,
`keyboard.type`, `page.screenshot`, `page.on('console')`, `page.route` (Fetch domain), `context.newPage` (Target.createTarget),
`page.close`, `browser.newContext` (isolated context) + `newPage`, `browser.close` — all 15 operations pass. Without
  `BOSON_PLAYWRIGHT_DIR` the test prints `SKIP playwright: ...`.
- Existing tests still pass (`ctest`: 15/15).

Not verified at runtime (implemented, unit-tested where noted, or only compiled):
- Focus handoff (Tab into / out of a guest), accelerator forwarding and `reservedaccelerators`: they need real keyboard input and
  window focus, which the tests avoid on a shared machine (key-name mapping and the forwarding rule are unit-tested).
- Server certificate errors and client certificate selection (need an HTTPS server with a bad / client certificate).
- `page-favicon-updated`, `audio-state`, `print` (dialog / printer), pause / resume of downloads, `openDevTools`.
- High DPI: the runtime run was at DPR 1; DPR scaling and rounding are unit-tested.
- CSS transforms / animations tracking, `z` restacking with several visible guests, composition hosting.
- `examples/webview_element` is built but was not launched (it would take the foreground on a shared machine).
- Puppeteer and other CDP clients; Playwright features beyond the list above (e.g. tracing, video, multiple simultaneous clients).
