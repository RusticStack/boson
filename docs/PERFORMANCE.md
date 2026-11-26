# Performance

Findings of the optimisation pass, one section per area. Numbers are from the machine described in
[BENCHMARKS.md](BENCHMARKS.md) (Windows 11 26H2, i7-13700KF, WebView2 158.0.4370.33) while other agents were building
and testing on it, so wall-clock figures carry real noise; allocation counts, bytes and wake-ups are exact and are the
numbers to trust.

## IPC hot path

Scope: WebMessageReceived string handling, Bridge, Router, TaskRunner / UiMarshal, glaze (de)serialisation, event emit,
PostWebMessageAsJson, the `frontend/src/boson.ts` runtime and (indirectly) the SyncBridge.

### How it was measured

- **`boson_ipc_bench`** (`tests/bench_ipc.cpp`, Release + LTO): replaces global `operator new` to count allocations and bytes
  across all threads, and counts UI wake-ups. It drives `Router::dispatch_sync`, the front half of `Bridge::on_web_message`
  (origin check + capability header peek), `Router::dispatch_async` -> stdexec pool -> `UiMarshal` -> drain (the bench thread
  plays the UI thread), `Bridge::emit_json` and the UTF-8/UTF-16 helpers. The final WebView2 COM call is not part of it.
  Five timed repetitions + one allocation-counted repetition per scenario; the table shows the median of 5 process runs
  (min-max of the ns column in brackets), baseline and new binaries run interleaved. The baseline is the same bench source built
  against the unmodified tree (a second worktree at commit 71c3757).
- **`boson_bench_app`** (examples/bench_app, driven by `boson_bench --load-signal`): real WebView2 end to end. It gained an
  `events_20k` scenario (20 000 events emitted from the pool, counted in the page, 3 repetitions) and a `--batch-events` flag.
  5 interleaved runs per variant.

### Native hot path, before -> after (per call)

| Scenario | ns/op base | ns/op new | allocs base -> new | bytes base -> new | UI wakes base -> new |
|---|---:|---:|---:|---:|---:|
| `Router::dispatch_sync` ping | 318 (224-378) | 271 (241-318) | 3 -> 1 | 1136 -> 64 | - |
| `dispatch_sync` void, no params | 214 | 195 | 2 -> 1 | 608 -> 64 | - |
| `dispatch_sync` echo 1 KiB | 1233 | 1086 | 7 -> 2 | 11 607 -> 2 128 | - |
| `dispatch_sync` echo 1 MiB | 2.97 ms | 1.41 ms | 7 -> 3 | 10.5 MB -> 5.2 MB | - |
| ping in a 32-method controller (last method) | 508 | 283 | 3 -> 1 | 1136 -> 64 | - |
| `Bridge::on_web_message` front half, capability policy | 321 | 182 | 0 -> 0 | 0 | - |
| front half, no policy | 154 | 67 | 0 -> 0 | 0 | - |
| async ping, sequential (pool round trip) | 17.9 us | 15.0 us | 10 -> 3 | 1712 -> 360 | 1 -> 1 |
| async ping, 1000 in flight | 814 | 482 | 7.1 -> 3 | 1639 -> 360 | 1.0 -> 0.002 |
| async echo 1 MiB, sequential | 4.10 ms | 2.12 ms | 15.2 -> 5 | 12.6 MB -> 6.3 MB | 1 -> 1 |
| `Bridge::emit_json` burst (per event) | 1153 | 701 | 4 -> 1 | 765 -> 744 | 1.0 -> 0 |
| `utf8_to_wide`, 64 B | 103 | 51 | 1 -> 1 | - | - |
| `wide_to_utf8`, 1 KiB | 481 | 205 | 1 -> 1 | - | - |
| `utf8_to_wide`, 1 MiB (fresh string) | 1.13 ms | 0.81 ms | 1 -> 1 | - | - |
| `utf8_to_wide_into`, 1 MiB (reused buffer) | n/a | 0.12 ms | 0 | - | - |
| `wide_to_utf8_into`, 1 MiB (reused buffer) | n/a | 0.19 ms | 0 | - | - |

(The sequential round trip is dominated by waking a sleeping pool thread and the bench's event wait, ~15 us, which
neither side changed; the allocation columns are exact, the ns columns are noisy. A fresh 1 MiB string costs ~0.6 ms of
page faults on its own, which is why the large-message paths reuse buffers.)

### End to end (real WebView2, `boson_bench_app`, 5 interleaved runs, median [min-max])

| Metric | base | new | new + `--batch-events` |
|---|---:|---:|---:|
| ping, 1000 sequential, mean (ms) | 0.098 [0.079-1.194] | 0.101 [0.070-0.155] | (same path) |
| 1000 concurrent pings (calls/s) | 75 188 [37 175-98 039] | 84 746 [64 103-100 000] | (same path) |
| events from the pool, page-counted (events/s) | 150 263 [18 531-175 747] | 160 901 [62 598-170 794] | **606 061 [520 833-754 717]** |
| 10 MiB upload (ms) | 126.2 [98.9-158.1] | 113.5 [97.1-219.0] | 95.8 [94.1-114.6] |
| 10 MiB download (ms) | 154.6 [124.6-157.8] | 126.6 [121.6-344.0] | 137.4 [125.3-142.0] |
| 10 MiB echo (ms) | 275.4 [247.6-290.5] | 239.6 [209.9-471.5] | 239.4 [222.4-415.3] |

Honest reading: ping latency and concurrent-call throughput did not move beyond the noise (the ~0.08 ms round trip is
mostly WebView2/Chromium message plumbing and a thread-pool hop; the native part is a few microseconds). What moved:
large payloads (-10 to -15 %), allocations, bytes, UI-thread wake-ups, and event throughput (x3.8 with batching, which is
opt-in). The 0.098 ms vs the 0.076 ms in BENCHMARKS.md is machine noise (the earlier run was on a quiet machine).

### What changed and why

1. **One parse instead of four.** A request used to be parsed by the capability peek, by `dispatch_async` (header, on the
   UI thread), and by `dispatch_sync` (full, owning copies of `method` and of the whole `params` text). Now nothing is parsed
   on the UI thread besides the allocation-free capability peek (only when a policy is attached);
   `dispatch_sync` reads `{id, method, params}` once into views (`glz::raw_json_view`) and parses `params` straight into the
   argument type. A request whose method needs unescaping (never produced by `JSON.stringify`) falls back to the old owning
   parse, so behaviour is unchanged.
2. **Method lookup from a compile-time sorted table.** Every reflected method of every controller becomes an entry
   (controller, method, thunk) in a `constexpr` array sorted at compile time; lookup is one binary search. Duplicate names
   keep the old "first controller wins" rule; `DynamicController`s (ElectronIpc) are tried after the table. 32-method
   controller, last method: 508 -> 283 ns.
3. **Response written once.** `{"id":..,"status":"success","data":..}` is serialised by glaze in one pass (`glz::obj`
   references the result), into a per-thread scratch string and copied out exactly sized (a ping response went from
   2 allocations / 608 B to 1 / 64 B; responses over 4 KiB move the scratch buffer out instead of copying). `make_success_envelope`
   and `make_event_envelope` are hand-built (byte-identical to the glaze output; a test compares them).
4. **No `std::function` / closure allocation on the dispatch path.** `TaskRunner::dispatch_reply` keeps the work callable in
   the stdexec operation state; the reply travels *inside* the `UiMarshal` queue item (`post_reply`), so the `done` closure
   (this, alive, id: fits `std::function`'s inline buffer) and the payload need no wrapper closure. The queue is one
   `std::vector` whose capacity is recycled between drains (it was two `std::deque`s of `std::function` / `std::string`).
5. **One UI wake per batch.** `UiMarshal` posts the wake-up only when none is pending; `drain_on_ui_thread` re-arms it. 1000
   in-flight pings: 1000 wakes -> 2 (0.002 per call). If `PostMessageW` ever fails (queue full) `Window::post_invoke` arms a
   10 ms retry timer so a coalesced wake cannot be lost. Items now run in strict FIFO order (previously all tasks ran before
   all payloads).
6. **Receive path without heap allocation.** `WebMessageReceived` converts the COM UTF-16 strings into stack buffers
   (`core::Utf8Text`, 768 B inline; larger messages reuse one per-thread heap buffer) instead of two `std::string`s, and the
   Bridge hands string views down. The buffer is NUL-terminated (glaze's default parse mode reads past the end otherwise).
7. **Allocation-free origin check.** `Bridge::is_origin_allowed` compared by building lower-cased copies of the origin and of
   every allowed entry per message; it now compares views case-insensitively (the dev-server origin `http://localhost:5173` is
   longer than the small-string buffer, so this was a real allocation per message). Same semantics, covered by the existing tests.
8. **Faster, reusable UTF-8 <-> UTF-16** (`core/text_convert.hpp`): SSE2 widen/narrow for pure-ASCII text (JSON is nearly
   always ASCII) with `MultiByteToWideChar` / `WideCharToMultiByte` for the rest, `*_into` variants that reuse a buffer, and a
   per-thread reusable wide buffer for `PostWebMessageAsJson`. A broadcast converts once for all WebViews. `utf8_to_wide` /
   `wide_to_utf8` keep their signatures and are faster for everyone. Mixed text (any non-ASCII character) takes the API path for
   the whole string, i.e. the old speed.
9. **Event emit**: one allocation (the envelope; the event name is written without a temporary), no closure allocation, and
   no wake-up per event (see 5).
10. **Opt-in event batching** (`BridgeAttachOptions::batch_events`, default off): the events emitted during one UI drain are
    delivered as one JSON array message per WebView, closed by a drain hook (`UiMarshal::add_drain_hook`) and flushed before any
    response is delivered, so responses never overtake events emitted before them. A lone event is sent unwrapped; batches are
    capped at 512 events / 256 KiB. 20 000 events: 160 k/s -> 606 k/s; 201 messages -> 19-44 for a 200-event burst in the test.
    **Wire compatibility:** with the option off (default) the wire format is exactly what it was; with it on, a page must
    unpack arrays. `frontend/src/boson.ts` does (`handleMessage` recurses into arrays), the `compat/*` shims and the transfer /
    gpu / terminal modules all go through `boson.on`, and the bench page does. Pages with their own raw
    `chrome.webview.addEventListener('message')` handler must handle `Array.isArray(event.data)` before enabling it.
11. **JS runtime**: an event with a single listener no longer copies the listener set (`[...set]`) per event.

### Not changed, and why

- **Capability header peek** still uses glaze (about 100 ns). It is a security decision; a hand-rolled scanner could be fooled by
  a `"method"` key inside `params` or a string, so it stays a real parse. `default`-off policy = no peek at all.
- **stdexec `async_scope::spawn`** allocates one operation state per call (~200 B). Avoiding it would mean bypassing the
  mandated execution framework with a hand-rolled queue; not worth it for ~40 ns.
- **Posting JSON as a string** (`postMessage(JSON.stringify(x))` + `TryGetWebMessageAsString`) to skip WebView2's own
  serialisation was not tried: it changes the wire format of every shim for an unmeasured gain.
- **BinaryTransport / StreamServer** already move bulk data through shared buffers and were not touched. The **SyncBridge** got no
  dedicated change; it benefits from the single-parse router and the allocation-free origin check (its cost is the COM
  host-object call, which blocks the page by design).
- A fresh 1 MiB request still costs two full copies on the way in (payload copy for the worker, params -> argument string) and
  one on the way out; a zero-copy 'params as a view into the WebView2 string' would need the UTF-16 -> UTF-8 conversion to
  happen on the worker, which would pin the COM string; not attempted.

### Behaviour notes for integrators

- `Router` dispatch order: static (reflected) methods are resolved first, `DynamicController`s afterwards. Previously the
  controllers were tried in declaration order, so a dynamic controller declared *before* a static one with the same
  `Controller.method` used to win; nothing in the tree does that.
- `TaskRunner::dispatch(callback_id, work, on_complete)` and `UiMarshal::post_task` keep their signatures; `dispatch_reply`,
  `post_reply` and `add_drain_hook` are new. `Router::dispatch_async` no longer parses on the calling thread and passes id 0
  to the TaskRunner's last-resort error path (the worker lambda catches everything itself and recovers the id).
- `WebView::post_web_message_as_json_wide` is new; `post_web_message_as_json` of an empty string now returns false instead
  of posting an invalid empty message.

## Verification

Verified (this machine, other agents running builds/tests concurrently):

- Release (MSVC, LTO, `/W4`) builds every target warning-free (full `build\rel` build, 0 warnings).
- Debug build of every target warning-free; `ctest`: 21 of 23 suites pass, including the new
  `boson_ipc_hotpath_tests` (2 668 checks: conversion fuzz against the Win32 APIs incl. SIMD/inline-buffer boundaries,
  router table / duplicates / dynamic / escaped method / 3 MiB round trip / error codes, envelope byte-equality with glaze,
  async dispatch, UiMarshal wake coalescing + FIFO + drain hooks, and real-WebView2 event batching: 200 events -> 19-44
  array messages in order with the response last; unbatched = exactly 201 plain messages).
  `boson_security_tests` (cleanup of the WebView2 user-data folder, `test_security.cpp:885`) and `boson_terminal_tests`
  (`processes().size() == 1` after Ctrl+C, `test_terminal.cpp:685`) fail here, and fail identically with the pre-existing
  executables built from the main checkout (`D:\Projects\Boson\build\wt`), so they are not caused by this work
  (`BrowserProcessExited not observed` / process-enumeration timing on a loaded machine). `boson_compat_typecheck` is
  disabled in this configuration (bun not run).
- The numbers above; 5 interleaved runs, baseline built from the unmodified commit.

- `frontend/src/boson.ts`: `bun run build` (`tsc --noEmit` + vite) passes; `boson_tsgen` output is identical to the committed
  `boson-bindings.d.ts` (the Router rewrite did not change the reflected API).

Not verified: the array unpacking in `boson.ts` itself was not executed in a page (the real-WebView2 test and the bench page use
their own listeners of the same shape); `boson_compat_typecheck` is disabled in this configuration; the Wails/Tauri/Electron comparison and the browser
scenario (`bench/run_browser_bench.ps1`) were not re-run, so the headline numbers in BENCHMARKS.md are unchanged; startup,
idle memory and binary size were not measured for this change (the change adds ~one small TU and removes none);
non-ASCII-heavy payloads fall back to the Win32 conversion and were only checked for correctness, not for speed; Release-mode
`ctest` was not run (Debug only).
