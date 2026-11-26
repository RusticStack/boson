# File Drop, Binary IPC and Streaming (transfer)

Three components live in `include/boson/transfer/`. Each is independent, and each attaches to WebViews without changing how the JSON `Bridge` works:

| Component | What it does |
|---|---|
| `FileDrop` | Native file drag-and-drop with real absolute paths. Covers drops inside a page (`postMessageWithAdditionalObjects` → `ICoreWebView2File::get_Path`) and drops on window areas that no WebView covers (`WM_DROPFILES`). |
| `BinaryTransport` | Zero-copy binary IPC both ways over `ICoreWebView2SharedBuffer`, with pooled buffers. Falls back to base64 inside JSON when the runtime lacks shared buffers. |
| `StreamServer` | Serves files or generated streams on `boson://media/...`. Supports HTTP Range (206/416, `Content-Range`, `Accept-Ranges`) for `<video>`/`<audio>` seeking. Bodies are streamed from an open handle and never loaded into memory whole. Directory mapping is safe against path traversal. |

The frontend helpers are in `frontend/src/transfer.ts`.

## How the hooks work

`WebView` gained two small extension points (in `webview.hpp` / `webview_events.cpp`). Both survive `Environment::recreate`.

- `add_web_message_observer(fn)`: the observer runs before the `Bridge` callback and receives the raw `ICoreWebView2WebMessageReceivedEventArgs*`, so it can reach `AdditionalObjects`. If it returns `true`, the message is consumed. Transfer messages always start with `{"__boson_transfer":`. Anything else returns `false` without being parsed, so JSON-RPC is unaffected.
- `add_resource_handler(filter, fn)`: adds a `WebResourceRequested` filter, which is re-added whenever the controller is re-created. The handler runs before the `boson://app` asset handler.

## FileDrop

```cpp
boson::transfer::FileDrop drop(window, [&](const boson::transfer::FileDropEvent& e) {
    // e.target: Window | WebView, e.paths (UTF-8, absolute), e.client_px, e.css_x/y,
    // e.webview_id, e.zone, e.metadata_json, e.source_uri, e.unresolved_objects
    runner.spawn([paths = e.paths] { /* index / open them off the UI thread */ });
});
drop.attach(webview);                      // after webview is ready; injects the page-wide handler
drop.attach(other, {.allowed_origins = {"https://app.example.com"}, .allow_external_drop = true});
drop.set_accept_window_drops(false);       // DragAcceptFiles off
```

- **Window drops.** The constructor calls `DragAcceptFiles` and allows `WM_DROPFILES` through UIPI, so files dropped from a non-elevated Explorer still arrive. `WM_DROPFILES` is handled through `Window::add_message_hook`. `client_px` comes from `DragQueryPoint`.
- **Page drops.**
  - `FileDrop::page_script()` reports files dropped anywhere in the page and prevents the default navigate-to-file. `attach` injects it automatically.
  - For named zones, use `createDropZone` from `transfer.ts`. It calls `preventDefault()`, so the page-wide handler skips drops a zone already handled.
  - C++ maps CSS coordinates to client pixels using the DPI, the zoom factor and the WebView bounds.
- **Origins.** Drops are accepted only from allowed origins. The default is the same as the Bridge's: `boson://app` plus the origin of `dev_url`.
- **After `Environment::recreate`.** Injected scripts are lost. To keep the handler, add `FileDrop::page_script()` to `WebViewConfig::scripts_on_document_created`.

```ts
import { createDropZone } from './transfer';
const off = createDropZone(document.getElementById('editor')!, { zone: 'editor', meta: { tab: 3 }, onDrop: (files) => console.log(files.map(f => f.name)) });
```

## BinaryTransport

```cpp
boson::transfer::BinaryTransport transport(runner, marshal, {.max_pool_bytes = 256 << 20});
transport.attach(webview);                                   // injects window.__bosonTransfer

transport.send_binary(webview.id(), "frame", bytes, R"({"w":1920,"h":1080})");  // any thread, one memcpy
transport.send_binary_with(webview.id(), "tile", size, [&](std::span<std::byte> out) { render_into(out); }); // no copy
transport.post_binary(webview.id(), "log", std::move(vec));  // copy happens on a pool thread

transport.on_upload("image", [](const boson::transfer::BinaryUpload& up) {   // pool thread
    // up.data views the shared memory the page wrote (up.zero_copy == true), valid during the call
    return std::string(R"({"stored":true})");                                 // JSON the page Promise resolves with
});
transport.on_request("thumbnail", [](const boson::transfer::BinaryRequest& req) {
    return boson::transfer::BinaryReply{.data = make_thumbnail(req.params_json), .metadata_json = R"({"format":"png"})"};
});
transport.mode(webview.id());   // SharedBuffer | Base64
transport.stats();              // created / reused / closed / in flight / idle bytes / sends / uploads / ack timeouts
transport.trim();               // close idle pooled buffers
```

```ts
import { onBinary, upload, requestBinary } from './transfer';
const off = onBinary<{ w: number; h: number }>('frame', ({ data, meta }) => draw(data, meta.w, meta.h)); // data valid during the call
const result = await upload<{ stored: boolean }>('image', fileBytes, { name: 'a.png' });
const thumb = await requestBinary('thumbnail', { id: 7 });                  // copy; or { zeroCopy: true } + thumb.release()
```

### Protocol and lifetime

**Native to page:**

1. The payload is copied into a pooled buffer. Size classes are powers of two from 64 KiB to 16 MiB, then multiples of 4 MiB.
2. The buffer is posted read-only with `ICoreWebView2_17::PostSharedBufferToScript`. `additionalData` carries `{tid, channel, length, meta}`.
3. The page runtime calls your listeners and `chrome.webview.releaseBuffer`, then posts `ack`.
4. The buffer goes back to the pool, unless the pool already holds more than `max_pool_bytes`; then it is closed.
5. A buffer that is not acknowledged within `ack_timeout` (default 60 s) is closed with `ICoreWebView2SharedBuffer::Close` and never reused.

**Page to native:**

1. `upload()` posts `alloc`.
2. Native posts a read-write buffer of that size.
3. The page writes into it once, releases it and posts `commit`.
4. The upload handler runs on a TaskRunner pool thread and reads the shared memory in place.
5. The buffer is recycled on the UI thread and the result is posted back.

Uploads larger than `max_upload_bytes` are rejected.

**Threads:**

- COM calls on buffers (create, post, close) happen on the UI thread only.
- From another thread, `send_binary` takes a pooled buffer without blocking. When none is free, it creates one through `UiMarshal::invoke_sync`, which blocks briefly. Never call it from a thread the UI thread is waiting on. `post_binary` never blocks the caller.

**Fallback:**

- The runtime is feature-detected with `ICoreWebView2Environment12` and `ICoreWebView2_17`. Without them, `set_force_base64(true)`, or for 0-byte payloads, everything is sent as base64 inside JSON web messages, with the same API on both sides. The page sees this as `mode: 'base64'`.
- If `PostSharedBufferToScript` fails for a single message, that message is re-sent as base64.

**Errors:**

- A handler that throws rejects the page Promise with `BosonTransferError` and the exception's message. Nothing crosses the ABI.
- Handler results that are not valid JSON are turned into errors.

`TransferController` (`include/boson/ipc/controllers/transfer_controller.hpp`, registered as `"Transfer"`) exposes `Transfer.capabilities()` and `Transfer.stats()` to JavaScript.

## StreamServer

```cpp
boson::transfer::StreamServer media(runner, marshal);            // host "media" -> boson://media/*
media.mount_directory("/videos", L"D:\Media");                   // boson://media/videos/clip.mp4
media.mount_generator("/live", [](const boson::transfer::StreamRequest& req) -> std::optional<boson::transfer::GeneratedResource> {
    if (req.path != "/feed.bin") return std::nullopt;             // 404
    return boson::transfer::GeneratedResource{
        .mime_type = "application/octet-stream",
        .size = 1 << 30,                                          // known size -> Range / 206 work
        .read = [](std::uint64_t offset, std::span<std::byte> out) { return fill(offset, out); }, // thread-safe
    };
});
media.attach(webview);
```

```html
<video src="boson://media/videos/clip.mp4" controls></video>
```

**Request handling:**

- Each request takes a `WebResourceRequested` deferral. A pool thread then resolves it: routes by the longest matching mount, checks the path, opens the file, stats it and builds the agile `IStream`.
- Only `CreateWebResourceResponse`, `put_Response` and `Complete` run on the UI thread, so the UI thread never touches the disk.
- The file stream reads with positioned `ReadFile` calls only when WebView2 pulls data.

**Responses:**

- Status codes: 200, 206 with `Content-Range`, 416 with `bytes */size`, `HEAD` (no body), `OPTIONS` (CORS preflight, 204), 405 for other methods, 404 and 403.
- Multi-range requests are answered with a full 200, which RFC 9110 allows.
- MIME types: `StreamServer::mime_type_for` covers mp4/webm/ogg/mov/mkv/mp3/m4a/wav/flac/opus/vtt/pdf and more, and falls back to the asset MIME table.

**Path safety:**

- `safe_join` percent-decodes the path. It rejects `..` (including encoded forms), backslashes, `:` (drive letters and alternate data streams), reserved device names (`CON`, `AUX`, `COM1`…), trailing dots or spaces, control characters, encoded NUL and invalid UTF-8.
- After opening, it compares `GetFinalPathNameByHandleW` of the handle with the root's final path. A junction or symlink inside the root that points outside it gets 403.

**Configuration:**

- `max_open_range_bytes` defaults to 0 (no cap). Keep it at 0 for media elements. Chromium's media loader does not use `Content-Range` for non-HTTP schemes, so a capped first answer makes it think the resource is that short: with a 1 MiB cap, a 60 s WAV reported 11.9 s.
- `allow_origin` (default `*`) and `cache_control` set the matching response headers.

## Verification

All numbers below were measured on this machine: Windows 11 26300, WebView2 runtime 158.0.4370.33, MSVC **Debug** build, `build\wt\boson_transfer_tests.exe`. That run passed **252/252** checks. `boson_tests` still passes, and `boson_integration_tests` passed 108/108 after the WebView hook changes.

**Tested headlessly:**

- base64: RFC 4648 vectors, round trips for sizes 0–299, and malformed input.
- Pool size classes.
- Range parsing, 16 cases.
- Percent decoding and traversal rejection, 20+ cases. `is_within`. MIME types.
- `StreamServer::resolve` against a temp directory:
  - 200, 206, suffix ranges, capped open ranges, 416, multi-range served as 200, `HEAD`, 404, 403, 405 and `OPTIONS`;
  - `IStream` `Seek` and `Stat`;
  - a real `mklink /J` junction escape, which got 403 and was removed afterwards;
  - generators with and without a known size.
- `FileDrop`: HDROP parsing, and a real `WM_DROPFILES` sent to a hidden `Window` with a hand-built `DROPFILES` block (paths, point, `set_accept_window_drops`), plus drop-message parsing.

**Tested against a real WebView2:**

- Shared-buffer feature detection, and the page runtime being injected.
- Native→page sends:
  - from the UI thread, from a worker thread (creates a buffer through `invoke_sync`), through `post_binary` and through `send_binary_with`;
  - content, checksums and metadata;
  - pool reuse after ack.
- base64 fallback for sends, requests and uploads.
- `requestBinary`: copy and zero-copy modes, handler exception, unknown channel.
- Shared-buffer upload with `zero_copy == true`, the empty upload, the base64 upload, and upload errors.
- Other transport behaviour:
  - an unacknowledged buffer expires after `ack_timeout`;
  - `trim` closes idle buffers;
  - a page from a disallowed origin is ignored;
  - JSON-RPC through the `Bridge` keeps working with the observers installed.
- `TransferController`.
- StreamServer through WebView2:
  - `fetch` with `Range: bytes=100-199` got 206 with the correct `Content-Range` and bytes;
  - a full fetch got 200;
  - `..%2f` traversal got 403, and a missing file got 404.
- `<audio>` playing a generated 60 s WAV: the duration was 60 s, and seeking to 50 s produced a real mid-file range request (`bytes=0-` then `bytes=4390912-`).

**Benchmark** (each operation is a full round trip):

- *Native→page*: from `send_binary` until the page listener has read the data and posted back. For shared buffers this includes waiting for the ack.
- *Page→native*: from `upload()` until the Promise resolves with the handler's result.

| Direction | Payload | JSON-base64 | Shared buffer |
|---|---|---|---|
| native → page | 1 MiB | 12.84 ms/op (77.9 MiB/s) | 0.38 ms/op (2600 MiB/s) |
| native → page | 64 MiB | 881.9 ms/op (72.6 MiB/s) | 24.6 ms/op (2600 MiB/s) |
| page → native | 1 MiB | 18.93 ms/op (52.8 MiB/s) | 0.71 ms/op (1399 MiB/s) |
| page → native | 64 MiB | 900.3 ms/op (71.1 MiB/s) | 20.3 ms/op (3153 MiB/s) |

Iterations: 20 at 1 MiB, 3 at 64 MiB, after one warm-up. Treat the numbers as indicative only: they come from a Debug build, from a single run, on a machine where other UI tests were running at the same time.

**Not verified:**

- **Real drag-and-drop of files from Explorer into a page**, and therefore `ICoreWebView2File::get_Path` returning a real path. A real drag needs user input (`SendInput` is not allowed here). A page cannot make a disk-backed `File` without a gesture: WebView2 rejects script-made Files with "additional File object is not a file on the disk", which the test logs. The test does check that a drop message is routed to `FileDrop` and kept away from the Bridge. A real drag onto a non-WebView area of the window was not done either; `WM_DROPFILES` was tested with a synthetic HDROP.
- **The runtime without shared-buffer support.** The base64 path was exercised with `set_force_base64`, not on an old runtime.
- **Elevated-process drops** (the `ChangeWindowMessageFilterEx` path).
- **Memory use while streaming large media.** Bodies are pulled from the file handle on demand, but how much WebView2 buffers internally for custom-scheme responses was not measured. Only `<audio>` was tested, not `<video>`.
- **Keeping injected page scripts across `Environment::recreate`.** They are re-added only if you put `page_script()` in `scripts_on_document_created`.
