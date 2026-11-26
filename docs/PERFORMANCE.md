# Performance

Findings of the optimisation pass, one self-contained section per area. Measurements are on Windows 11 Pro 10.0.26300,
Intel Core i7-13700KF (24 logical CPUs), MSVC 19.52 (VS 18), WebView2 runtime 158.0.4370.33. Other agents built and ran
UI tests on the same machine the whole time, so wall-clock numbers carry heavy noise; sizes (bytes), section/object
contributions and process counts are exact and are the primary evidence in this section.

## Build configuration, binary size and codegen

### Result in one paragraph

Boson executables were already small (the linker drops every unreferenced module of `boson_core`). The one codegen
switch that wins clearly is **RTTI off in Release (`/GR-`, now the default)**: -11 to -17 % executable size, no
behavioural change (nothing uses `dynamic_cast`/`typeid`; 22/22 ctest suites pass in that configuration).
`/Gw`, `/Zc:inline`, `/Gy`, `/arch:AVX2` and PGO produced **no measurable wall-clock, CPU or memory difference** in
the Boson host process, so they are not defaults (PGO and AVX2 stay as opt-ins; PGO does shrink the file by 8-10 %).
A one-file **single executable** build (`-DBOSON_SINGLE_EXE=ON`) imports only Windows system DLLs: 618 KB for a hello
app, 1.46 MB for the demo.

### Baseline (before any change)

Release + LTO, `/MD`, `/O2 /Ob3`, `/OPT:REF,ICF`, `/DEBUG:NONE` (already in the tree). Sizes in bytes:

| Exe | Baseline |
|---|---:|
| `boson_demo.exe` | 1,370,624 |
| `boson_browser.exe` | 1,290,752 |
| `boson_bench_app.exe` | 765,952 |
| `boson_hello.exe` (new minimal app, `examples/hello`) | 485,376 |

### Where the bytes go (`boson_demo.exe`, `link /MAP`, LTCG)

| Section | Size |
|---|---:|
| .text | 900 KB (0xDC000) |
| .rdata | 324 KB (0x51000) |
| .data | 104 KB (0x1A000; 83 KB of it is `.data$rs`, glaze/zmij/fast_float tables) |
| .pdata + .xdata (unwind) | 30 KB + 62 KB |
| .reloc + .rsrc | 8 KB + 4 KB |

Largest contributors by object (publics, size to the next symbol): `main.cpp.obj` 578 KB (63 % of .text: the demo's
controllers, `Router` dispatch lambdas, glaze reader/writer instantiations and the two embedded assets), then
`boson_core` objects, largest first: `webview_events` 79 KB, `updater` 74 KB, `environment` 62 KB, `window_state` 59 KB,
`crash_reporter` 40 KB, `store` 39 KB; the static WebView2 loader is 23 KB; `msvcprt` (iostream/locale/filesystem pieces)
is about 26 KB. Single largest data objects: glaze `itoa_40kb` digit table 40 KB, embedded demo JS 17.6 KB, fast_float
powers 10.4 KB, zmij constants 10 KB + 5 KB. Nothing is individually worth removing: the demo's 63 % is the price of
reflecting 20+ controllers, and per-controller glaze instantiations are not duplicated (each controller's `read`/`write`
specialisations exist once; ICF merges the identical thunks).

### Modules the app does not use are dropped

`boson_core` is one static library, but the linker only pulls the objects an executable references, and
`/OPT:REF,ICF` removes the rest. Measured: `boson_hello` (window + WebView only) is 402 KB against 1.21 MB for
`boson_demo`, while both link the same library. No split into per-module libraries was needed (and none was done):
there is nothing left to drop. `boson_gpu` remains a separate library because it adds d3d/dwrite imports.

### Experiments (one change at a time, Release + LTO, `/MD`, bytes)

| Configuration | hello | demo | browser | bench_app | Verdict |
|---|---:|---:|---:|---:|---|
| baseline | 485,376 | 1,370,624 | 1,290,752 | 765,952 | |
| + `/Gw /Zc:inline` | 485,376 | 1,370,624 | 1,290,752 | n/a | byte-identical size: under `/GL` + `/OPT:REF` they change nothing. Not set. (`/Gy` is implied by `/O2`.) |
| + `/GR-` (RTTI off) | 402,432 | 1,209,856 | 1,119,232 | 648,192 | **-17.1 % / -11.7 % / -13.3 % / -15.4 %**. Default now. |
| + `/Os` instead of `/O2` | 413,696 | 1,125,376 | 1,115,648 | 662,528 | demo -17.9 % but slower code is likely; not default (`-DBOSON_RELEASE_CODEGEN_FLAGS=-Os`) |
| + `/arch:AVX2` | 486,912 | 1,370,112 | 1,290,752 | 763,904 | no size change, no measurable speed change; opt-in `-DBOSON_AVX2=ON` (default stays baseline x64) |
| `/GR-` + PGO (`bench/pgo.ps1`) | n/a | n/a | 1,003,008 | 594,432 | -10.4 % / -8.3 % vs `/GR-` alone, speed not measurably different. Opt-in `-DBOSON_PGO=GEN/USE` |

`/favor:` was not evaluated: it only retunes instruction selection for a specific micro-architecture and the AVX2 and PGO
runs already showed no measurable difference in the host process. clang-cl was not tried (not installed with this
VS component set; optional in the task).

### Startup, IPC and host cost: A/B, 6 interleaved runs each

`boson_bench --exe <app> --wait-ready --load-signal` on `boson_bench_app`, medians with [min-max]. Host-process numbers are
the app process itself (not WebView2) after the idle phase.

| Variant | First window (ms) | Ready / first frame (ms) | Sequential IPC mean (ms) | Host CPU after startup+idle (ms) | Host private WS (MB) | Host commit (MB) |
|---|---:|---:|---:|---:|---:|---:|
| baseline (`/MD`, RTTI) | 42.6 [23-85] | 439 [406-2800] | 0.18 [0.07-0.37] | 62.5 [15.6-78.1] | 3.2 | 4.5 |
| `/GR-` (new default) | 39.4 [26-46] | 446 [391-568] | 0.12 [0.09-0.15] | 46.9 [31.3-62.5] | 3.2 | 4.5 |
| `/GR-` + PGO | 50.9 [30-58] | 495 [358-539] | 0.21 [0.07-0.34] | 78.1 [31-172] | 3.1 | 4.4 |
| `/Os` | 60.0 [34-101] | 560 [404-933] | 0.17 [0.12-0.27] | 78.1 [47-125] | 3.1 | 4.5 |
| `/arch:AVX2` | 45.4 [33-63] | 541 [413-956] | 0.28 [0.12-0.38] | 78.1 [31-94] | 3.1 | 4.5 |

Reading: every spread overlaps. The time to a first web frame (~440 ms) and the IPC round trip (~0.1-0.2 ms) are dominated
by WebView2's browser/renderer processes and the page-to-host marshalling, not by Boson's code generation; CPU time is
quantised to the 15.6 ms scheduler tick. Handles (248) and process count did not change. Conclusion: none of these switches
earns a place in the default configuration on speed; only size (RTTI) does.

### Not done, with the reason

- **Asset compression (brotli/gzip `Content-Encoding`)**: the embedded frontend of the demo is 25.7 KB in total (17.6 KB JS,
  8.1 KB CSS); gzip would save roughly 18 KB of a 1.2 MB executable (1.5 %) and add an inflate call on every request, and
  whether a `WebResourceResponse` with `Content-Encoding` is decoded by WebView2 was not verified. Assets come from memory, so
  there is no network to save. Not worth it.
- **Embedded injected scripts**: nothing is injected by default (`Bridge::attach` adds no document-created script).
  Opt-in scripts: `BinaryTransport` page runtime 6.2 KB, `FileDrop` 0.9 KB, guest runtime 1.4 KB, sync-IPC boot values
  (tens of bytes). Leading indentation is 11 % of the largest; a constexpr minifier would save about 0.9 KB of JS source per
  navigation and about 1.7 KB of `.rdata` (the literals are `wchar_t`). Below noise, so not added.
- **Frontend bundles**: already separate entry points. Minified (bun, no tree-shake context): `boson.ts` core 2,988 B,
  `compat/wails` 23.4 KB, `compat/electron` 25.8 KB, `compat/tauri` 13.4 KB, `webview_element` 19.2 KB, `terminal` 9.6 KB,
  `process` 9.4 KB, `native` 7.3 KB, `windows` 6.6 KB. No module imports a compat shim; an app that imports only
  `boson.ts` ships ~3 KB (the demo, which uses the core plus its own UI, is 17.6 KB JS + 8.1 KB CSS).
- **iostream/locale**: only 10 library sources include `<fstream>/<sstream>/<format>`; the whole `msvcprt` contribution is
  about 26 KB (2 % of the demo), so replacing them was not attempted.
- **Static CRT size**: see "Single executable" below (+17 % for hello with `/GR-`, +21 % for demo).

### Recommended release configuration

```bat
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release            :: LTO (/GL, /LTCG), /O2 /Ob3, /GR-, /OPT:REF,ICF
cmake -S . -B build\single  -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOSON_SINGLE_EXE=ON   :: + static CRT, import check
bench\pgo.ps1 -BuildDir build\pgo                                             :: optional, -8..-10 % size, speed unchanged
```

Defaults for Release now: LTO, `/GR-` (`-DBOSON_NO_RTTI=OFF` if your own code uses `dynamic_cast`/`typeid`), `/O2 /Ob3`,
`/OPT:REF /OPT:ICF /DEBUG:NONE`. Opt-in knobs: `BOSON_SINGLE_EXE`, `BOSON_STATIC_CRT`, `BOSON_PGO=GEN|USE`, `BOSON_AVX2`,
`BOSON_RELEASE_CODEGEN_FLAGS`. `CMakePresets.json` has `release`, `single-exe`, `single-exe-bootstrapper`, `pgo-gen`, `pgo-use`.

### PGO pipeline

`bench/pgo.ps1` (or `-DBOSON_PGO=GEN`, train, `pgomgr /merge`, `-DBOSON_PGO=USE`): instrumented `boson_bench_app` and
`boson_browser` are trained with `boson_bench` (startup, first frame, IPC load, tabs, resize, window moves), profiles merged to
`<BuildDir>/pgo/<target>.pgd`, then only the link step is redone (the `/GL` objects are reused). Facts found while building it:
`/GENPROFILE:PGD=` writes no `.pgd` unless a PDB is produced, so PGO builds link with `/DEBUG` instead of `/DEBUG:NONE`;
the instrumented exe needs `pgort140.dll` from the compiler's `bin` directory on `PATH`; counters land next to the exe as
`<exe>!N.pgc`. The profile used 3.2 % (bench_app) / 5.5 % (browser) of the reserved counter space, 0 % overflow.

## Single executable

Details, prerequisites, bootstrapper licence notes and limits are in [SHIPPING.md](SHIPPING.md#single-executable).

Sizes (bytes; ZIP = .NET Deflate "Optimal", one file), Release + LTO + `/GR-`:

| Exe | `/MD` (needs VC++ redist) | Single exe (`/MT`) | Single exe, ZIP | + embedded bootstrapper | UPX `--best --lzma` |
|---|---:|---:|---:|---:|---:|
| `boson_hello.exe` | 402,432 | **618,496** | 281,791 | 2,445,824 (ZIP 2,004,244) | 227,328 |
| `boson_demo.exe` | 1,209,856 | **1,463,808** | 642,366 | n/a | 464,896 |
| `boson_browser.exe` | 1,119,232 | **1,355,264** | 591,787 | n/a | 438,784 |

The embedded bootstrapper is Microsoft's 1,826,528-byte `MicrosoftEdgeWebview2Setup.exe` (already compressed, so the ZIP barely
shrinks). For reference `wails_browser.exe` in this repository is 10,578,432 bytes (4,292,581 zipped, see BENCHMARKS.md).
Tauri v2 and Electron single-binary sizes were **not available** (no comparison numbers were produced for them while this
section was written); Electron's runtime alone is far above 100 MB.

UPX experiment (documented only, not a default): files shrink 63-68 %, but the first-window time of the hello app went from
49 ms [30-235] to 172 ms [61-292] (median of 6 interleaved runs) and its private working set grew from 2.2 to 2.7 MB, because
the packed image is unpacked in full at start-up and can no longer be demand-paged. Packed executables are also a frequent
antivirus false positive (not measured here). Do not ship UPX'd builds unless download size matters more than start-up.

Dependency check: `ctest -R single_exe_deps` (`cmake/BosonCheckDependents.cmake`) runs `dumpbin /dependents` on the produced
exe and fails for anything outside the allow-list of Windows system DLLs (and `api-ms-win-*`). `boson_hello` imports 13 DLLs
(ADVAPI32, ole32, SHLWAPI, dwmapi, dcomp, USER32, GDI32, SHELL32, CRYPT32, WINHTTP, WINTRUST, KERNEL32,
api-ms-win-core-synch); the same check run against the `/MD` build fails and names `MSVCP140.dll;MSVCP140_ATOMIC_WAIT.dll;
VCRUNTIME140_1.dll;VCRUNTIME140.dll`.

## Verification

Verified:

- Release + LTO builds of `boson_hello`, `boson_demo`, `boson_browser`, `boson_bench_app`, `boson_bench` in six configurations
  (baseline, `/Gw /Zc:inline`, `/GR-`, `/Os`, AVX2, single exe), zero compiler warnings. The whole tree (365 targets) built in the
  `/GR-` Release configuration.
- `ctest` of that full Release `/GR-` build: 22/22 pass (a first pass had 3 `integration` failures - environment re-creation and
  guest-crash tests, which kill and relaunch WebView2 processes while other agents were using the machine; all three passed on
  an immediate re-run).
- Single exe: `ctest -R single_exe_deps` 3/3 pass (hello, demo, browser) and, as a negative control, fails on the `/MD` hello.
  `boson_hello.exe` (single) starts, creates its window (title "Boson Hello") and a WebView2 browser process tree on this machine.
- Missing-runtime handling: with `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` pointing at an empty folder (the loader then finds no
  runtime), the plain single exe and the embedded-bootstrapper exe each open a native dialog (window class `#32770`, title
  "Boson Hello") instead of failing silently. `tests/test_shipping.cpp` additionally covers: an embedded bootstrapper is never
  downloaded and unsigned bytes are rejected (`VerificationFailed`) before running, `require_webview2_runtime` returns false
  without dialogs when silent, and true when a runtime is installed.
- PGO: full GEN, train, merge, USE cycle ran through `bench/pgo.ps1`; both instrumented and optimised apps ran their scenarios.

Not verified:

- **Windows Sandbox**: `WindowsSandbox.exe` is not present on this machine (the optional feature is not enabled), so the
  single exe was **not** run on a clean Windows. The dependency check is static (imports), not a clean-machine run.
- The embedded bootstrapper was never actually **executed to install** a runtime (this machine has one; clicking "Yes" was
  avoided so as not to alter it). Its write-to-temp, Authenticode check and launch path is the same code as the downloaded
  bootstrapper, which `boson_shipping_tests` covers up to the signature check.
- Redistribution licence for the bootstrapper: Microsoft's distribution documentation describes shipping the Evergreen
  bootstrapper with an app as the supported way to cover the missing-runtime case, but this pass could not find the licence
  wording programmatically; read the terms at the download page before shipping it inside your exe.
- Wall-clock speed claims: none are made; differences were inside the noise (table above).
- Debug-build ctest (`boson_build.bat`, 383 targets, zero warnings): 20/22 pass first time (`boson_shipping_tests`,
  `boson_integration_tests` etc. included). `boson_security_tests` (314/315) and `boson_process_tests` (397/398) fail one check
  each, `!fs::exists(base)` after "Environment teardown: BrowserProcessExited not observed" (WebView2 teardown timeout leaves the
  profile folder locked on this loaded machine). The security test fails identically with an unrelated, independent build at
  `D:\Projects\Boson\build\wt`, and both passed in the Release `/GR-` run, so they are environmental, not caused by this
  change; `boson_store_tests` failed once under load and passed on re-run.
- clang-cl, `/favor`, asset compression, JS minification: not done (reasons above).
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
Findings of the optimisation passes, one self-contained section per area. Method for all of them: measure before, change
one thing, measure after, keep only what wins or is neutral and simpler. The development machine (Windows 11 26300,
i7-13700KF, WebView2 158) was shared with other builds and test runs the whole time, so wall-clock numbers swing by
2-5x between runs. Where possible the tables therefore also give **counts of expensive operations** (window moves,
calls into the browser process), which do not depend on load.
## Multi-view layout, rendering and streaming throughput
Release + LTO (`build\rel`) unless stated. "Before" is commit 71c3757 (main) built the same way.
### 1. Native child-window layout (`<boson-webview>`, `WebViewHost`)
**Where the time went** (instrumented run of the existing `apply_layout` benchmark, 9 guests, per call average):
`SetWindowPos` of the container ~0.96 ms, `ICoreWebView2Controller::put_Bounds` ~0.77 ms (a synchronous round trip into
the browser process), `NotifyParentWindowPositionChanged` ~0.03 ms when the browser process is idle (it is also a cross-process
call and grows under load), `put_IsVisible` ~1.9 ms, `ShowWindow` ~0.3 ms. The old code issued SetWindowPos + put_Bounds +
Notify for every guest on every layout, and `WebViewHost::relayout` / the browser example did it for every tab, hidden
ones included, plus a redundant `ShowWindow` + `put_IsVisible` + `SetWindowPos(HWND_TOP)` for the shell on every layout.
**What changed** (`src/core/webview.cpp`, `include/boson/core/webview.hpp`, `src/webview/guest.cpp`):
| Change | Effect |
|---|---|
| `core::BoundsBatch` (BeginDeferWindowPos / DeferWindowPos / EndDeferWindowPos); used by `GuestManager::apply_layout`, `WebViewHost::relayout` (default layout) and `examples/browser` | N container moves become one window-manager pass; falls back to per-window `SetWindowPos` if the batch cannot be built (other parent, API failure) |
| Container rect cached; `SetWindowPos` only when it changes | 0 calls for unchanged WebViews |
| `put_Bounds` only when the controller size changed | a pure move (scroll, tab strip drag) no longer pays the synchronous round trip (the controller's bounds are relative to its container) |
| `NotifyParentWindowPositionChanged`: immediate for the first move after a quiet period, then at most one per 50 ms per WebView, with a trailing call 50 ms after the last move (`WM_TIMER` on the container) | continuous scroll/drag costs 1 cross-process call per burst instead of one per frame; popups opened mid-scroll get their position 50 ms after the scroll stops |
| Bounds of a **hidden** windowed WebView are stored, applied by `set_visible(true)` | a window resize with 10 tabs touches 1 tab instead of 10 |
| `set_visible` / `apply_visibility` skip `ShowWindow` and `put_IsVisible` when the state is unchanged; `bring_to_front` returns early when the container is already the top content window | idle-state layouts (the shell calls both every layout) issue 0 calls |
Calls per layout frame, N visible guests moving (no size change), from the code paths and confirmed with the call counters
of the instrumented run (before: 3152 SetWindowPos, 3152 put_Bounds, 3152 Notify for the same number of guest placements):
| | SetWindowPos | put_Bounds | Notify | ShowWindow + put_IsVisible + bring_to_front |
|---|---:|---:|---:|---:|
| before | N | N | N | 1 each per layout for a shell that re-asserts its state |
| after | 1 batch (N entries) | 0 | 0 inside a burst (1 leading + 1 trailing per WebView) | 0 |
| resize (N visible) | 1 batch | N (needed) | N | 0 |
| window resize, T tabs of which 1 visible | 1 batch | 1 | 1 | 0 (before: T of each) |
`GuestManager::apply_layout`, 9 guests (`BOSON_WE_ONLY=element,perf boson_webview_element_tests`), us/frame, medians [min..max],
6 runs each, interleaved A/B in the same minute (machine loaded):
| | move | move + clip | resize | unchanged |
|---|---:|---:|---:|---:|
| before (legacy path, same binary, switch removed afterwards) | 12 547 [7 875..17 098] | 17 231 [9 603..24 168] | 13 387 [10 927..18 585] | 0.2 |
| after (batch + skip put_Bounds + throttled notify) | 4 364 [3 038..10 237] | 13 908 [8 187..21 605] | 12 015 [7 910..14 958] | 0.2 |
| after, later 5 runs in a quieter period | 2 600 [2 500..6 200] | 5 000 [4 650..8 500] | 6 000 [5 700..12 700] | 0.15 |
Move is ~3-4x cheaper; resize is bound by `put_Bounds` (one synchronous browser-process round trip per resized guest),
which cannot be removed; move + clip is bound by `SetWindowRgn(TRUE)` per guest (the redraw is needed to repaint what the
clip uncovers). The < 0.5 ms/frame target was **not** reached: `EndDeferWindowPos` itself costs ~0.1-0.25 ms per window here
(measured: 1.9-2.5 ms for 9 windows under load; `SWP_NOCOPYBITS` / `NOREDRAW` changed it by < 20 %, within noise, and `NOREDRAW`
would leave stale pixels, `SWP_ASYNCWINDOWPOS` was 2x slower) because the window manager updates the cross-process child window
tree of every moved container. The remaining lever is moving fewer guests per frame (hide scrolled-away guests: they cost 0).
Browser scenario (`boson_bench --scenario browser`, Release+LTO, 5 runs + 1 warm-up, base and new interleaved, machine heavily
loaded - cold-start rows swung 8x between runs and are meaningless here), Window rows:
| Metric | before | after |
|---|---:|---:|
| move: content follows after (ms, median) | 1.76 [1.40-1.90] | 1.67 [1.32-2.37] |
| move: p95 (ms) | 2.83 | 2.32 |
| resize: content follows after (ms, median) | 7.36 [5.43-8.35] | 6.81 [5.23-28.14] |
| 60 Hz live resize: frames in sync (%) | 100 | 98 |
| 60 Hz live resize: catch-up (ms) | 16.1 [6.6-54] | 29.8 [4.5-65] (ranges overlap; noise) |
**Resize vs Wails (3.7 vs 6.9 ms before)**: an instrumented run (10 tabs, 2 WebViews resized: active tab + shell) split a
resize into ~0.7 ms `EndDeferWindowPos`, ~1.3 ms for the two `put_Bounds`, ~1.4 ms of redundant shell calls (now removed) and
~3 ms outside `layout()`: the cross-thread `WM_WINDOWPOSCHANGING` / `WM_NCCALCSIZE` / `WM_SIZE` delivery to the app thread plus the other
resize listeners (titlebar, caption buttons, window state). Wails' hook calls `SetWindowPos` straight from its move/size callback. The
layout part (~3.4 ms -> ~2 ms expected) is reduced; the measured end-to-end median moved 7.36 -> 6.81 ms, inside the noise, so the gap
to Wails is **not closed**. The remaining ~3 ms is outside the layout code and was not attributed further.
### 2. Terminal throughput (ConPTY)
The pipeline after ConPTY is not the limit: a synthetic 256 MiB source through `OutputChannel` -> shared buffers -> page with acks runs
at 2.4-4.2 GB/s (229 MiB/s once under heavy load); a keystroke -> echo round trip through the whole stack is 0.7-1.0 ms. Output
coalescing (send at once when idle, gather while a frame is in flight, 16 ms / 64 KiB bound) and ack credit were reviewed and left
unchanged: throughput is bounded by ConPTY, latency by one frame only when the renderer is busy.
The in-box (kernel32) ConPTY paints at ~60 Hz and caps output at ~15-20 MiB/s; the redistributable `conpty.dll` + `OpenConsole.exe` does not.
`Pty` already loaded it when it sat next to the executable; what was missing was a supported way to ship it.
**New: `cmake/BosonConPTY.cmake`** (opt-in): `-DBOSON_BUNDLE_CONPTY=ON` for the tests/demo, or in your own project
`include(cmake/BosonConPTY.cmake)`, `boson_bundle_conpty(my_app)`, `install(FILES ${BOSON_CONPTY_FILES} DESTINATION .)`.
It downloads `Microsoft.Windows.Console.ConPTY` from nuget.org at configure time, **rejects the package unless its SHA-256 matches
the pinned value**, extracts it and copies `conpty.dll` and `OpenConsole.exe` next to the target after every build. Checked for
version 1.25.261109001 (the pinned default): licence expression `MIT` in the nuspec (copyright Microsoft Corporation); `conpty.dll`
(119 608 bytes, sha256 e971856f...f492) and `OpenConsole.exe` (1 089 336 bytes, sha256 b17e48b0...a775) both verify with
`signtool verify /pa` (chain: Microsoft Code Signing PCA 2024 -> Microsoft Corporation, timestamped); package sha256
`3d77f535df848e44225c2a8912ac844fecb23f3057732b639f3034b587496f09`. Ship the licence text of the Windows Terminal repository
(MIT) with your app. The default stays OFF: the files add 1.2 MB and a second process per terminal.
Release `boson_terminal_tests`, in-box vs bundled, 2 interleaved runs each (heavily loaded machine, so the in-box column varies 7x):
| | in-box | bundled |
|---|---:|---:|
| `echo` command round trip (cmd.exe) | 15.8, 15.2 ms | 0.71, 0.34 ms |
| 128 MiB end to end (child -> ConPTY -> ring -> shared buffer -> page, acked) | 2.4, 17.5 MiB/s | 226.5, 241.6 MiB/s |
| 64 MiB raw ConPTY, no renderer (200x50) | 3.4, 7.5 MiB/s | 51.1, 19.0 MiB/s |
| synthetic source -> page (pipeline ceiling) | 229, 4 209 MiB/s | 3 497, 2 452 MiB/s |
The raw 200x50 figure is far below the end-to-end one even for the bundled backend and below the 296-308 MiB/s in
docs/TERMINAL.md (Debug, quieter machine); not investigated (differences: terminal size and the child's output pattern).
In the Release runs `boson_terminal_tests` reported one failure in both configurations, `test_terminal.cpp:685 (shell->processes().size() == 1)`
(a timing-dependent process-count check on the loaded machine; the Debug run of the same test passed 236/236, and the terminal sources are untouched).
### 3. GPU surfaces (`gpu::Renderer2D`, `gpu::Surface`)
Reviewed, **no code change**: the numbers do not justify one.
- The 1.46 ms for 17k quads was a Debug figure. Release: **0.24 ms** CPU per frame for the 240x70 text grid on D3D11 (0.20 ms on D3D12),
  17 150 quads in 2 draw calls (0.44-0.47 ms D3D11 including GPU completion). That is ~14 ns per quad. `draw_text_grid` allocates nothing
  per frame (the pending vector is reserved once), the constant buffer is a 16-byte `UpdateSubresource` once per frame, and the instance
  buffer is one `Map(WRITE_DISCARD)` per 16 384 quads. Writing quads straight into the mapped buffer would save one 64-byte copy per quad
  (~0.05-0.1 ms); not done, because it complicates every state-change flush for a gain far below the 1 ms target.
- Present pacing (read, plus the existing test): one `Present` per rendered frame; `SetMaximumFrameLatency(1)` with the waitable object, a core
  renders only after `WaitForSingleObject(waitable)`/`WaitForMultipleObjects` handed out its slot, and the loop sleeps (`INFINITE`) when no
  surface wants a frame. Animated surface: 181 fps vsync-paced on this display, render-thread CPU 0.39-0.52 ms per frame, 0.35-0.73 ms
  for the last frame callback + present.
### Verification
Verified: Release (LTO) and Debug builds with `/W4` have no warnings; all 22 enabled ctest suites pass in the Debug build (`boson_compat_typecheck` is disabled); `boson_webview_element_tests` passes (202/202 in the `element,perf` run, with new
checks that batched moves land exactly and that a hidden WebView applies its stored bounds on show); the A/B tables above were produced
from the same builds; the CMake helper downloaded, hash-checked and installed the package and the bundled terminal run used it
(`backend: conpty.dll`).
Not verified: the cost of `EndDeferWindowPos` on a quiet machine (every run had other builds running); the effect of the trailing
position notification on popups opened mid-scroll (reasoned, not exercised); `BoundsBatch` when a listener destroys a WebView inside
`GuestManager::apply_layout` between staging and commit (not possible from the current `place()` code, which runs no user callbacks);
composition hosting (the batch applies to windowed hosting only); ARM64; packaged install with `BOSON_CONPTY_FILES`.
