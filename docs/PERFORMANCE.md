# Performance

Each optimization area of the framework keeps a self-contained section here: what was measured, what changed, what was
rejected, and what was **not** verified. Numbers are from one machine (below); wall-clock figures are noisy because other
workloads ran on it, so each section leans on counts (threads, handles, modules, bytes) that do not depend on load.

Test machine: Windows 11 Pro 10.0.26300 (26H2), Intel Core i7-13700KF (24 logical CPUs), 48 GB RAM, WebView2 runtime
158.0.4370.33, MSVC 14.52, **Release** (`/O2 /Ob3`, `/OPT:REF,ICF`, no PDB) build in `build\rel`.

## Memory footprint, threads and idle cost

### Scope and method

Question: what does a Boson app cost *while doing nothing*, which part of that is Boson (the host process) and which is the
WebView2 runtime, and what in the host is paid for but never used?

- **Apps measured**: `boson_bench_app` (minimal: window + WebView + bridge), `boson_demo` (the showcase), and the new
  `boson_kitchen_sink` (examples/kitchen_sink): bench_app's page plus **every** module linked, constructed and registered
  with the Router: crash reporter, logger, store, in-memory database, scoped fs, titlebar, clipboard, global shortcuts,
  display watcher, permissions, tray, process manager, binary transport + stream server, terminal host, drag-out, window
  manager, guest manager, updater, menu backend, and 28 controllers (everything except `GpuController`, which needs a
  `gpu::SurfaceHost` and has its own lazy-device design, see docs/GPU.md).
- **Scenarios**: idle (4 s settle, then a 12 s window), after a burst (the bench page's IPC load test: 1000 sequential + 1000
  concurrent calls, 3 x 10 MB transfers), and minimized (main window minimized, 3 s settle, 10 s window).
- **Tool**: `bench/memsnap.ps1` (new). Starts the exe with a throw-away user data folder and records, for the host process:
  private working set, commit (`PrivateUsage`), handle count, thread count, loaded-module count, heap count (Toolhelp),
  per-thread context switches (`NtQuerySystemInformation(SystemProcessInformation)`), plus the same sums for the whole
  process tree. **No ETW session was used**; per-thread context-switch counters and thread counts are the "wake-up
  snapshot" (ETW needs admin and perturbs a busy machine). All A/B runs were **interleaved** (A, B, C, A, B, C ...) and the
  tables show median (min-max).
- Host = the app's own process. Tree = host + all `msedgewebview2.exe` processes.

### Where the memory is (before any change)

| | Host process | WebView2 processes |
|---|---:|---:|
| bench_app, idle | 3.3 MB private WS, 4.7 MB commit, 33 threads, 287 handles, 44 modules | ~80 MB private WS, ~143 MB commit, 6 processes, ~140 threads, ~3400 handles (tree total: 84 MB, 147 MB, 7 processes, 175 threads, 3670 handles) |
| kitchen sink, idle | 4.5 MB private WS, 6.5 MB commit, 36 threads, 334 handles, 56 modules | same |

The host is ~4 % of the tree's private working set (before this pass). Everything the framework owns is therefore worth a few MB at most;
the remaining lever is the runtime's own configuration (see "WebView2 options"). Idle wake-ups of the host were already
negligible before this pass: bench_app 0.0 context switches/s (median, 0-4.7), kitchen sink 0.3-0.9/s, and nearly all of
it is the UI thread servicing WebView2. The measurements show no polling in the host (kitchen sink: every module constructed, 0.3-0.9
wake-ups/s, all on the UI thread). A grep for timed waits found only the gpu device idle wait (5 s), the fs-watch shutdown
poll (50 ms) and the WebView2-runtime installer poll (200 ms), none of them part of steady state; this is not an
exhaustive audit of every timer the optional modules can arm once they are in use. The demo's 15-40 context switches/s and ~80 % of a core are its page (60 fps rAF animation + 1 Hz tick), not the
framework.

### Changes that won

All three are in this branch; the first two were A/B-tested one at a time on the same sources.

#### 1. Right-sized default thread pool (`exec::Scheduler`)

`Scheduler` used `max(2, hardware_concurrency())` workers: 24 threads on this machine, all created at startup
(`stdexec::static_thread_pool` has no lazy growth). IPC handlers are short and never run on the UI thread, so the extra
threads were pure idle cost. The default is now `clamp(hardware_concurrency / 2, 2, 6)`
(`Scheduler::kMaxDefaultThreads`); an explicit `Scheduler(n)` is unchanged for workloads that block pool threads.

Isolated experiment (same binary, pool size forced with a temporary env override, 3 interleaved runs each; deterministic):

| Pool threads | Host threads | Host private WS (KB) | Host commit (KB) | Host handles |
|---:|---:|---:|---:|---:|
| 2 | 11 | 2472-2484 | 3552-3560 | 265 |
| 6 | 15 | 2624-2648 | 3756-3780 | 269 |
| 24 (old default) | 33 | 3368-3456 | 4788-4880 | 287 |

Each worker costs ~40 KB private WS, ~56 KB commit and one handle for the life of the process. IPC does not
need the extra threads. bench_app `--auto` (the IPC test of docs/BENCHMARKS.md), old default (24 workers) vs 6 workers, 6
interleaved runs each on a quiet machine, median (min-max):

| Metric | 24 workers | 6 workers |
|---|---:|---:|
| 1000 sequential calls, p50 / mean (ms) | 0.10 / 0.094 | 0.10 / 0.095 |
| 1000 concurrent static calls (calls/s) | 90 909 (77 519-94 340) | 96 154 (80 000-100 000) |
| 1000 concurrent dynamic calls (calls/s) | 90 090 (85 470-92 593) | 94 340 (75 758-98 039) |
| 10 MB upload, median of 3 (ms) | 73.4 (70.0-83.2) | 78.4 (70.6-81.3) |
| 10 MB download (ms) | 108 (97-161) | 100 (97-106) |
| 10 MB echo (ms) | 245 (165-261) | 169 (166-178) |

(An earlier set of rounds taken while other agents were compiling was too noisy to rank 2 / 8 / 24 threads at all.) A side effect: with fewer workers a 1 Hz `runner.spawn` wakes fewer threads: the demo's
host context-switch rate fell from 40 (23-64) to 15 (9-33) per second.

Rejected: *lazy* pool creation. Every Boson app dispatches its first IPC at page load, so deferring the threads only moves
a ~1.5 ms cost; there is no steady-state gain.

#### 2. Delay-loaded system DLLs (`BOSON_DELAYLOAD`, default ON)

`boson_core` links ~25 system libraries; the linker keeps each one an ordinary import as soon as any object that uses it is
pulled in, so `LoadLibrary` ran for all of them before `main`. They are now delay-loaded through PUBLIC link options on
`boson_core` (consumers inherit them; `delayimp` is linked, `/IGNORE:4199` silences "no imports from X" for executables
that never reference a listed DLL): `uiautomationcore, dcomp, windowscodecs, crypt32, bcrypt, winhttp, wintrust, wtsapi32,
ws2_32, runtimeobject, propsys`. First use resolves the import (a single `LoadLibrary`, system DLLs only).

Deliberately **not** delay-loaded:
- `dbghelp`: the crash reporter documents that its crash path must be fully loaded at install time; loading a DLL from
  inside an unhandled-exception filter (loader lock, possibly a stack overflow) is exactly what a crash handler must not do.
- `user32, gdi32, ole32, oleaut32, shell32, advapi32, dwmapi, shcore, shlwapi, version`: mapped by the window,
  the WebView2 loader or the shell before `main` anyway, so delaying them only adds a call-time stub.
- `d3d11, d3d12, dxgi, dwrite` of `boson_gpu`: apps that link `boson_gpu` use it immediately; apps that do not link it
  never reference them (already lazy by library split).

Result (kitchen sink): `dcomp, uiautomationcore, winhttp, ws2_32` (and `Microsoft.Internal.WarpPal`, a dependency of
dcomp) are no longer mapped at startup. `crypt32, wtsapi32` (and `propsys`, which the WebView2 loader maps itself)
are still loaded in the kitchen sink because some module calls them while it is being constructed
(`WTSRegisterSessionNotification` in `PowerMonitor`; the `crypt32` caller was not traced). The executables grow by ~3-4 KB (bench_app 765 952 -> 768 512
bytes, kitchen sink 6 130 688 -> 6 134 784).

Cost: the first call into a delayed DLL pays its load (typically 1-5 ms) on whichever thread makes it, e.g. the first
UI Automation probe loads `uiautomationcore` on the UI thread. The first-frame time did not move: see "Startup".

`tests/test_boson.cpp::test_delay_load_policy` parses the PE import tables of the test executable and fails if any listed
DLL is an ordinary import, if `dbghelp` is delay-loaded, or if the delay table is empty (not vacuous).

Combined idle result (5 interleaved runs per cell, 12 s window, median (min-max)):

| App | Build | Threads | Modules | Handles | Host private WS (MB) | Host commit (MB) | Tree private WS (MB) |
|---|---|---:|---:|---:|---:|---:|---:|
| kitchen sink | before | 36 (36-36) | 56 (56-56) | 334 (332-335) | 4.45 (4.32-4.54) | 6.50 (6.32-6.66) | 85.8 (83.5-87.2) |
| kitchen sink | + pool 6 | 18 (18-18) | 56 (56-56) | 316 (316-317) | 3.43 (2.86-3.54) | 5.10 (5.01-5.23) | 84.3 (53.1-87.5) |
| kitchen sink | + delay-load (final) | 18 (18-18) | 51 (48-51) | 305 (281-305) | 3.20 (3.07-3.30) | 4.82 (4.62-4.90) | 84.3 (82.3-87.3) |
| demo | before | 36 (36-36) | 47 (47-47) | 339 (339-341) | 3.82 (3.78-3.88) | 5.50 (5.45-5.54) | 118.5 (109.2-124.6) |
| demo | + pool 6 | 18 (18-18) | 47 (44-47) | 297 (273-297) | 2.97 (2.82-3.02) | 4.35 (4.17-4.39) | 118.8 (112.0-121.8) |
| demo | + delay-load (final) | 18 (18-18) | 44 (44-44) | 292 (292-293) | 2.88 (2.77-2.91) | 4.19 (4.07-4.21) | 120.6 (118.5-122.4) |
| bench_app | before | 33 (33-33) | 44 (44-44) | 287 (287-287) | 3.29 (3.27-3.32) | 4.66 (4.65-4.70) | 84.9 (82.4-86.9) |
| bench_app | + pool 6 | 15 (15-15) | 44 (41-44) | 269 (245-270) | 2.58 (2.38-2.64) | 3.69 (3.46-3.74) | 83.6 (80.3-86.3) |
| bench_app | + delay-load (final) | 15 (15-15) | 42 (42-42) | 266 (266-267) | 2.55 (2.50-2.57) | 3.63 (3.56-3.65) | 83.3 (81.4-85.9) |

Thread snapshot, bench_app: 33 -> 15 threads. The 18 removed threads are the idle pool workers (24 -> 6). Of the 15 left, 1
is the UI thread, 6 are pool workers and 8 are not created by Boson code (WebView2 client / COM / RPC runtime inside the
process; classified by subtraction, see "Not verified"). The kitchen sink has 3 more (18): the crash-reporter writer and
the logger writer (dedicated, blocking, zero-wake-up threads, kept on purpose: the crash writer must exist before the
crash; the logger writer could start on the first log line, but an app that creates a Logger logs at startup, so that would
save nothing in practice) and one thread that was not identified.
Idle context switches of the host after the change: bench_app 0 (0-13) per second, kitchen sink 0.9 (0.5-6.4), demo 15
(10-45, page-driven).

#### 3. Low memory target while minimized (`WebViewConfig::low_memory_when_minimized`, default true)

`WebView::set_parent_minimized` already hid the controller (`IsVisible = false`), but a hidden WebView2 kept its
working set (measured: minimized == not minimized). It now also asks the runtime for `MemoryUsageTargetLevel = Low`
(`ICoreWebView2_19`) while the parent is minimized and returns to the requested level on restore.
`WebView::applied_memory_target()` reports what the runtime has; `memory_usage_target()` stays what the app asked for;
`set_keep_visible_when_minimized(true)` (media / DRM apps) opts out automatically. Page state is untouched: only the
runtime's caches are trimmed.

Minimized window, tree private working set, 10 s window after 3 s settle, interleaved:

| App | Before | After | Delta |
|---|---:|---:|---:|
| bench_app (n=5) | 84.9 MB (83.1-86.1) | 77.0 MB (74.9-77.4) | -7.9 MB (-9 %) |
| kitchen sink (n=3) | 83.8 MB (83.5-84.0) | 75.7 MB (75.7-76.1) | -8.1 MB (-10 %) |

Commit and the host's own working set are unchanged (the runtime trims working set, not commit). Idle CPU while minimized
stayed at noise level in both (0-0.7 % of a core). Test: `tests/test_integration.cpp` ("suspend / resume / memory target",
real WebView2) asserts Low on minimize, Normal on restore, and that keep-visible opts out.

After a burst of IPC activity (3 runs each) the host settles at +0.8-1.0 MB private WS vs idle and returns to ~0 wake-ups/s;
the tree returns to ~197 MB only because the bench *page* keeps its 10 MB strings alive (not host memory). Host private WS
after the burst: bench_app 4.09 -> 3.10 MB, kitchen sink 5.06 -> 3.78 MB, i.e. the same savings as idle.

### Startup

`boson_bench --wait-ready`, 8 interleaved runs per binary, before vs final build:

| App | First window (ms) before / after | Ready (first web frame, ms) before / after |
|---|---:|---:|
| bench_app | 26 (17-31) / 33 (15-38) | 330 (310-368) / 317 (279-347) |
| kitchen sink | 30 (19-43) / 34 (22-35) | 316 (270-371) / 323 (277-336) |

No measurable change either way (the differences are inside the run-to-run spread). The pool is ~1.5 ms of thread creation
and the delayed DLL loads are a few ms; neither is visible next to WebView2's ~300 ms environment + first navigation.

### WebView2 options evaluated

Tree totals for bench_app, idle, `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` set per run, 3 interleaved runs per row
(same binary):

| Option | Tree private WS (MB) | Commit (MB) | Processes | Threads | Handles | Verdict |
|---|---:|---:|---:|---:|---:|---|
| none | 84 (83.3-85.8) | 147.6 | 7 | 175 | 3670 | baseline |
| `--disable-features=msSmartScreenProtection` | 84.3 | 146.7 | 7 | 171 | 3674 | no gain; **not enabled** (SmartScreen is a security feature) |
| `--renderer-process-limit=1` | 83.9 | 148.3 | 7 | 175 | 3706 | no gain with one page; matters only with many windows |
| `--disable-crash-reporter --disable-breakpad` | 85.1 | 148.6 | 7 | 175 | 3703 | no gain, hides runtime crashes |
| `--disable-features=Translate,msEdgeShoppingUI,AutofillServerCommunication,OptimizationHints,...` | 83.3 | 145.9 | 7 | 170 | 3666 | within noise |
| `--disable-background-networking` | 85.6 | 148.0 | 7 | 175 | n/a | no gain |
| `--disable-sync`, `--disable-default-apps` | 86.0 / 85.3 | 149.4 / 148.6 | 7 | 176 | n/a | no gain |
| `--disable-component-update` | **80.9** (80.8-81.0) | 143.9 | 7 | 174 | n/a | -4.5 MB, **opt-in only**: it also stops CRL sets and the Widevine/PlayReady component updates DRM playback depends on (docs/COMPOSITION_DRM_SPIKE.md) |
| `--in-process-gpu` | **69.9** (69.7-81.1) | 128.8 | **6** | 160 | 3293 | -14 MB, -1 process, -377 handles, **opt-in only**: a GPU crash then takes the browser process (and the app's WebView) down, and the GPU sandbox is weakened; unsupported by Microsoft |

None of the "disable feature" switches is enabled by default: they did not reduce memory beyond noise, or they trade
security / reliability for it. The two that do (`--disable-component-update`, `--in-process-gpu`) are one line for an app
that accepts the trade-off:

```cpp
boson::core::EnvironmentConfig env;
env.additional_browser_arguments.emplace_back(L"--disable-component-update"); // -4.5 MB; no CRLSet / CDM updates
env.additional_browser_arguments.emplace_back(L"--in-process-gpu");           // -14 MB, one process less; GPU crash = browser crash
```

Already in place and verified to exist, not changed here: `WebViewHost::set_sleep_policy` puts a *hidden* WebView to sleep
after a timeout (`MemoryUsageTargetLevel Low` + `TrySuspend`); multi-window apps share one environment (one browser
process tree) through `app::WindowManager`.

### Evaluated and rejected

- **One shared message-only window for all hidden-window needs.** The kitchen sink owns 3 Boson hidden windows
  (display watcher: top-level because WM_DISPLAYCHANGE is only broadcast to top-level windows; clipboard listener and
  global shortcuts: message-only) plus the tray window, all created on first use of their module (a bench_app has none).
  Merging the two message-only ones would save two user objects (hundreds of bytes of desktop heap each) and no measurable
  host memory, at the price of coupling unrelated modules' window procedures. Not done.
- **Heap tuning.** The process heap is already low-fragmentation on this Windows (heap count 5, host private WS 2.5-3.2 MB
  total), and releasing host caches on minimize has nothing to release. The only meaningful trim is the WebView2 one above.
- **Delay-loading `dbghelp`** (crash safety, above) and **a lazy thread pool** (no steady-state gain).
- **Static initializers / large tables:** the host's whole private working set is ~2.6 MB, so no table of significance
  can exist; not pursued further.

### Reproducing

```bat
cmake -S . -B build\rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build\rel
powershell -File bench\memsnap.ps1 -Exe build\rel\boson_kitchen_sink.exe -Name ks -Scenario idle      -Idle 12 -Out out\ks_idle.json
powershell -File bench\memsnap.ps1 -Exe build\rel\boson_kitchen_sink.exe -Name ks -Scenario burst     -Idle 10 -Out out\ks_burst.json
powershell -File bench\memsnap.ps1 -Exe build\rel\boson_kitchen_sink.exe -Name ks -Scenario minimized -Idle 10 -Out out\ks_min.json
rem old behaviour for A/B:  cmake -DBOSON_DELAYLOAD=OFF ...   and   boson::exec::Scheduler scheduler(24);
```

### Verification

Verified:
- Release (`build\rel`, `/W4`) builds all targets with zero warnings, including `/DELAYLOAD` + `delayimp` (no LNK4199).
- Debug (`build\wt`) full build is warning-free; `ctest` passed 22 of 22 enabled suites (`boson_compat_typecheck` is
  disabled in this checkout). `boson_desktop_tests` failed once under `ctest -j3` on the *system clipboard* section
  (another process held the clipboard) and passed on both serial re-runs.
- New checks: Scheduler sizing (`test_scheduler_sizing`), delay-load import-table policy, minimized memory target on a real
  WebView2 (integration test).
- Every table above is raw output of `bench/memsnap.ps1` / `boson_bench`; interleaved, median (min-max).

Not verified:
- ETW traces (not used; counters from `NtQuerySystemInformation` instead). Thread *names* are not resolved: the Release
  build has no PDB, so the 15/18 remaining threads are classified by count and by elimination, not by symbol.
- Which thread is the 18th of the kitchen sink, and which 8 non-Boson threads the runtime creates in-process.
- The latency of the first call into a delay-loaded DLL (expected 1-5 ms, not measured per DLL), in particular the first
  UI Automation client (screen reader) probe.
- Behaviour with other WebView2 runtime versions, other Windows versions, or machines with a different core count (the
  default pool size is a function of `hardware_concurrency`; only the 24-thread machine was measured).
- Visual smoothness when restoring a minimized window after Low (no dropped-frame measurement; the level is restored in the
  same call that makes the controller visible again).
- A GPU-using app (`boson_gpu`) with the new link options: `boson_gpu_tests` pass, no GPU-specific idle measurement.
- Comparisons with Tauri / Wails / Electron are outside this section.
