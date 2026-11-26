# Benchmarks

Three pieces live in this repository:

- **`boson_bench`** (tools/bench/) — a framework-agnostic harness. It launches any executable, follows its whole
  process tree (Toolhelp parent-PID walk, unioned with a job object it creates around the target, so
  `msedgewebview2.exe`, Electron renderer/GPU/utility processes and crashpad helpers are all included, even when a
  child breaks away from the job), and samples private working set, working set, commit (private bytes),
  handle count and CPU time of the whole tree.
- **`boson_bench_app`** (examples/bench_app/) — a minimal Boson app with an in-page IPC load test.
- **`bench/compare/`** + **`bench/run_compare.ps1`** — the same reference app in Boson, Electron, Tauri v2 and Wails v3 with one harness
  and one command (see "Boson vs Electron vs Tauri v2 vs Wails v3" below).

## What is measured

| Metric | How |
|---|---|
| First window (ms) | Launch (`CreateProcessW`) -> first visible, non-empty top-level window owned by any process of the tree (polled every 5 ms) |
| Ready (ms) | Optional: the app writes a `QueryPerformanceCounter` value to `%BOSON_BENCH_DIR%\ready`. `boson_bench_app` does it when the page's second `requestAnimationFrame` fires, i.e. after the first frame of web content (closest available proxy for first paint) |
| Idle | After a settle period (default 3 s) the tree is sampled every 500 ms for 30 s. Reported: mean/max of private WS, commit, WS, handles, process count, and tree CPU time consumed during the window as % of one core |
| Load | Optional: `--load-signal` posts the registered message `BosonBench.StartLoad` to the target's windows and waits for `%BOSON_BENCH_DIR%\load_done`; the app may leave `load_result.json`, which is embedded in the report. `--load-command "<cmd>"` instead runs an external driver (any framework) and samples until it exits |
| Shutdown (ms) | `WM_CLOSE` to the root's windows -> every tracked process exited (killed after 20 s, flagged `(killed)`) |

CPU time of processes that exit during a phase is kept (handles are held open), so short-lived helpers are counted.
Private working set is `PROCESS_MEMORY_COUNTERS_EX2::PrivateWorkingSetSize` — the number Task Manager shows as "Memory".

## Running

```bat
rem Boson bench app: startup + 30 s idle + IPC load, 3 runs
build\wt\boson_bench.exe --exe build\wt\boson_bench_app.exe --name boson_bench_app --out bench-results ^
    --runs 3 --idle 30 --settle 3 --wait-ready --load-signal --cooldown 5 --no-samples

rem Boson demo: startup + idle only (it has no load protocol)
build\wt\boson_bench.exe --exe build\wt\boson_demo.exe --name boson_demo --out bench-results --runs 3 --idle 30 --no-samples

rem Side-by-side table of several reports
build\wt\boson_bench.exe --compare bench-results\boson_bench_app.json bench-results\boson_demo.json --out bench-results\comparison.md
```

Other flags: `--interval MS`, `--startup-timeout S`, `--load-timeout S`, `--arg <a>` / `-- <args...>` (passed to the
target). Without `--no-samples` the JSON keeps every raw sample for plotting. The target is started with
`SW_SHOWNOACTIVATE` and never receives focus; it is killed with the harness's job object if the harness dies.
`boson_bench_app --auto` runs the IPC test once without the harness and writes
`%TEMP%\boson_bench_app_result.json`.

## Results on this machine (measured 2026-11-26)

- Windows 11 Pro 10.0.26300 (26H2), Intel Core i7-13700KF (24 logical CPUs), 48 GB RAM, WebView2 runtime 158.0.4370.33.
- **Debug build** (MSVC, `/W4`, no optimisation) — the only configuration built for this workstream. Native-side
  numbers (the app process itself) should be lower in Release; the WebView2 processes are unaffected by our build type.
- Other agents were building and running UI tests on the same machine at the time, so expect noise.
- 3 runs each, 30 s idle after 3 s settle, 500 ms sampling.

### Startup and idle

| App | First window (ms, median) | Ready / first frame (ms, median) | Idle private WS (MB) | Idle commit (MB) | Idle handles | Processes | Idle CPU (% of one core) |
|---|---:|---:|---:|---:|---:|---:|---:|
| boson_bench_app | 31 | 288 | 77.9 | 142.3 | 3704 | 7 | 0.36 |
| boson_demo | 16 | n/a (no ready hook) | 107.5 | 211.1 | 3770 | 7 | 38.60 |

Per run:

| App | Run | First window | Ready | Idle private WS | Idle commit | Idle CPU % | Shutdown (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| boson_bench_app | 1 | 32 | 288 | 73.2 | 137.7 | 0.36 | 388 |
| boson_bench_app | 2 | 31 | 327 | 77.9 | 142.3 | 0.68 | 282 |
| boson_bench_app | 3 | 29 | 228 | 78.3 | 142.7 | 0.10 | 326 |
| boson_demo | 1 | 18 | n/a | 110.5 | 229.6 | 40.50 | 311 |
| boson_demo | 2 | 16 | n/a | 107.5 | 205.6 | 32.22 | 310 |
| boson_demo | 3 | 15 | n/a | 107.3 | 211.1 | 38.60 | 325 |

Where the memory goes (boson_bench_app, run 3, end of idle):

| Process | Private WS (MB) | Commit (MB) | Handles |
|---|---:|---:|---:|
| boson_bench_app.exe (the Boson app itself) | 3.4 | 4.8 | 249 |
| msedgewebview2.exe — browser | 22.9 | 31.1 | 1243 |
| msedgewebview2.exe — 5 children (renderer, GPU, network/storage utilities, crashpad) | 2.1 / 27.7 / 5.9 / 2.7 / 14.5 | 3.5 / 62.6 / 10.9 / 7.4 / 23.4 | 209 / 1262 / 337 / 183 / 325 |

Interpretation:

- The Boson host process is ~3.4 MB private working set; > 95 % of the tree is the WebView2 runtime.
- "First window" for Boson is the native window, shown before WebView2 starts; "Ready" is the honest
  time-to-first-web-frame (~0.23–0.33 s here).
- The demo's idle CPU is **not** framework overhead: its frontend runs a continuous 60 fps
  `requestAnimationFrame` ring animation + FPS counter and a 1 Hz backend tick event. In run 3 the renderer used
  7.9 s and the GPU process 5.0 s of CPU over the run. The bench app's page is static and idles at 0.1–0.7 % of one
  core for the whole tree. The demo also uses Mica and a larger page (more private memory).

### IPC (boson_bench_app, JS -> C++ -> JS through Bridge + Router + TaskRunner)

Latencies are measured in the page with `performance.now()`, which WebView2 coarsens to 100 µs, so per-call
figures are quantised. 50 warm-up calls precede each measurement.

| Test | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| 1000 sequential small calls, static controller (`Bench.ping`) — mean / p50 / p99 ms | 0.235 / 0.2 / 0.6 | 0.254 / 0.3 / 0.4 | 0.190 / 0.2 / 0.4 |
| 1000 sequential small calls, dynamic `ElectronIpc.invoke` — mean / p50 / p99 ms | 0.261 / 0.2 / 0.6 | 0.285 / 0.3 / 0.5 | 0.224 / 0.2 / 0.4 |
| 1000 concurrent small calls, static — calls/s | 17 036 | 13 106 | 11 862 |
| 1000 concurrent small calls, `ElectronIpc` — calls/s | 15 974 | 11 507 | 11 001 |
| 10 MB string JS -> C++ (`Bench.upload`), median of 3 — ms (MB/s) | 227 (44.0) | 210 (47.7) | 194 (51.6) |
| 10 MB string C++ -> JS (`Bench.download`), median of 3 — ms (MB/s) | 248 (40.3) | 247 (40.4) | 220 (45.5) |
| 10 MB echo round trip (`Bench.echo`), median of 3 — ms (MB/s) | 493 (20.3) | 465 (21.5) | 377 (26.5) |

The dynamic `ElectronIpc` path (glz::generic parse + handler lookup) costs ~0.03 ms per call over the reflected
static path here. Large payloads are dominated by WebView2's own `PostWebMessageAsJson` / `postMessage`
marshalling (the page-to-host string crosses a process boundary), not by glaze.

## Boson vs Wails v3: multi-tab browser (issue #18)

### What is compared

- **Boson**: `examples/browser` (`boson_browser.exe`), Release with LTO (`build\rel`), started with `--bench`.
- **Wails v3.1.0**: `bench/wails_browser` (`wails_browser.exe`), built like `wails3 build` does for production
  (`-tags production -trimpath -ldflags "-w -s -H windowsgui"`), Go 1.27.4.
- **Wails, tuned** (`wails_debounce1`): the same binary with `--move-debounce=1`
  (`WindowsWindow.WindowDidMoveDebounceMS = 1`, down from the default 50 ms).

Both apps are the same product: a frameless 1280x860 window with a 40-DIP header shell (Mica, dark), one WebView per
tab below the header, and tab sleeping. The shell runs the same measurement code (`bench/shared/bench_shell.js`) and
the tabs load the same static page (`bench/shared/bench_tab.html`, 48 cards), each from the app's own embedded assets.
Differences that come from the frameworks and that matter when reading the numbers:

| | Boson | Wails v3 |
|---|---|---|
| Tab WebView | a child of the main window (`WebViewHost`), one shared environment | a frameless top-level window per tab (Wails has one WebView per window), owned by the main window and moved from the `WindowDidMove` / `WindowDidResize` hooks with a plain `SetWindowPos` |
| Sleeping a tab | `SleepPolicy`: lower the memory target, then `TrySuspend`; the renderer stays alive, state is kept | the public API has no `TrySuspend`, so a sleeping tab's window is closed (page state lost) and recreated when the tab is activated |
| IPC | `chrome.webview.postMessage` -> Bridge -> reflected `Router` on a worker pool | Wails runtime `Call.ByName` (HTTP fetch to the asset server) |
| Events | `Bridge::emit` to the shell WebView | `app.Event.Emit`, which broadcasts to every window (all tab windows too) |

### Method

`boson_bench --scenario browser` (tools/bench/scenario.hpp) launches each app with `BOSON_BENCH_DIR` set; the app runs a
fixed scenario and stops at checkpoints, where the harness measures and then lets it continue (file protocol: the app
writes `checkpoint`, the harness answers `ack`):

1. **Cold start**: launch -> first visible window, -> shell first paint (double `requestAnimationFrame` in the
   shell, QPC stamp), -> first tab `NavigationCompleted`. The WebView2 runtime and the binaries are in the OS file
   cache (warm start, not first boot).
2. **Memory** at 1, 5 and 10 tabs, and at 5 and 10 tabs after every hidden tab is put to sleep: 2 s settle, then the
   median of 5 samples 250 ms apart. "Host private bytes" is the app process's commit; "WebView2 tree working set" is
   the summed working set of every `msedgewebview2.exe` in the tree (it double-counts shared pages, as Task Manager's
   per-process numbers do); "whole tree private WS" is the summed private working set of everything.
3. **Window**: the harness moves and resizes the frame with `SetWindowPos` (from its own thread, as a drag would)
   and busy-polls `GetWindowRect` of the active tab's content window until it is back at its offset (+-1 px):
   20 discrete moves, 12 resizes, then a 60 Hz "drag" and "live resize" of 60 steps each, checking one frame after
   every step whether the content is in place. This measures the geometry of the content HWND, not pixels on screen.
4. **IPC** (in the shell): 1000 sequential pings (after 50 warm-up calls), 1000 concurrent pings, 5 x 1 MiB string
   echo, and a burst of 10 000 events emitted from the backend and counted in the page.
5. **Tabs**: new tab -> `NavigationCompleted`; tab switch (20, between awake tabs); wake of a sleeping tab;
   destroy + recreate (5). Switch, wake and recreate are timed until the tab page has rendered two frames after
   becoming visible (the host posts a mark, the page answers after a double `requestAnimationFrame`).

5 measured runs per app after 1 warm-up run, interleaved (boson, wails, wails_debounce1, boson, ...) with a 3 s
cooldown. Cells are **median [min-max] over the 5 runs**. Every run of every app completed; tabs asleep were 4 of 4 at
5 tabs and 9 of 9 at 10 tabs in every run.

Re-run everything (builds both apps, runs the scenario, measures sizes) from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File bench\run_browser_bench.ps1            # -Runs 5 -Warmup 1 by default
powershell -ExecutionPolicy Bypass -File bench\run_browser_bench.ps1 -SkipBuild # reuse build\rel and bin\
```

Reports land in `build\bench-results\browser\` (`<app>.scenario.json` with every run, `<app>.scenario.md`,
`comparison.md`, `sizes.md`). The Wails build needs Go and the Wails v3 module (`bench/wails_browser/build.ps1`).

### Results (measured 2026-11-26)

- Windows 11 Pro 10.0.26300 (26H2), Intel Core i7-13700KF (24 logical CPUs), 48 GB RAM, WebView2 runtime 158.0.4370.33.
- Boson: MSVC 14.52, Release, `/O2`, LTO (`/GL` + `/LTCG`), `/MD`. Wails v3.1.0, Go 1.27.4.
- **Quiet machine**: final run with no other builds or tests running (5 measured runs + 1 warm-up per app,
  interleaved). Medians with [min-max]. Single outliers remain: one Wails cold start took ~20.8 s and one Boson
  shutdown took ~17.6 s (WebView2 browser-process exit); the medians are not affected.

| Group | Metric | boson | wails | wails_debounce1 |
|---|---|---:|---:|---:|
| Startup | first window (ms) | 38 [12-383] | 256 [215-20793] | 289 [200-415] |
| Startup | shell first paint (ms) | 306 [242-817] | 407 [292-20917] | 411 [307-543] |
| Startup | first tab NavigationCompleted (ms) | 261 [207-768] | 364 [250-20909] | 349 [257-496] |
| Memory tabs_1 | host private bytes (MB) | 4.7 [4.7-4.8] | 52.0 [51.8-52.3] | 52.2 [52.1-52.8] |
| Memory tabs_1 | WebView2 tree working set (MB) | 358.5 [355.6-363.0] | 378.3 [375.2-379.4] | 376.5 [376.1-377.8] |
| Memory tabs_1 | whole tree private WS (MB) | 100.1 [97.6-101.2] | 110.2 [107.1-110.7] | 108.2 [107.6-108.2] |
| Memory tabs_1 | processes | 8 [8-8] | 8 [8-8] | 8 [8-8] |
| Memory tabs_5 | host private bytes (MB) | 7.0 [6.7-7.2] | 65.4 [65.2-67.4] | 65.7 [65.1-66.7] |
| Memory tabs_5 | WebView2 tree working set (MB) | 647.9 [641.0-649.6] | 812.1 [808.4-816.2] | 808.7 [744.0-814.1] |
| Memory tabs_5 | whole tree private WS (MB) | 219.8 [216.6-224.4] | 366.5 [363.8-369.9] | 363.7 [285.4-370.1] |
| Memory tabs_5 | processes | 12 [12-12] | 12 [12-12] | 12 [12-12] |
| Memory tabs_5_slept | host private bytes (MB) | 7.0 [6.7-7.2] | 63.9 [63.7-65.8] | 64.5 [64.3-65.7] |
| Memory tabs_5_slept | WebView2 tree working set (MB) | 479.9 [459.5-482.7] | 531.3 [529.6-534.5] | 528.6 [482.8-529.9] |
| Memory tabs_5_slept | whole tree private WS (MB) | 184.2 [170.3-185.6] | 264.3 [262.5-266.7] | 262.3 [206.1-264.4] |
| Memory tabs_5_slept | processes | 12 [12-12] | 8 [8-8] | 8 [8-8] |
| Memory tabs_10 | host private bytes (MB) | 7.4 [7.2-7.6] | 65.2 [64.0-67.2] | 66.0 [65.5-67.1] |
| Memory tabs_10 | WebView2 tree working set (MB) | 799.7 [791.7-800.3] | 1073.5 [1030.4-1079.0] | 1066.7 [1020.4-1075.1] |
| Memory tabs_10 | whole tree private WS (MB) | 263.7 [261.2-266.9] | 401.3 [346.2-405.3] | 395.8 [337.3-401.0] |
| Memory tabs_10 | processes | 17 [17-17] | 17 [17-17] | 17 [17-17] |
| Memory tabs_10_slept | host private bytes (MB) | 7.4 [7.2-7.6] | 64.0 [63.7-66.3] | 65.3 [64.8-67.0] |
| Memory tabs_10_slept | WebView2 tree working set (MB) | 546.9 [486.7-549.1] | 497.3 [492.8-500.4] | 496.3 [489.4-537.0] |
| Memory tabs_10_slept | whole tree private WS (MB) | 217.7 [188.5-219.8] | 221.1 [217.2-223.7] | 221.5 [212.9-270.9] |
| Memory tabs_10_slept | processes | 17 [17-17] | 8 [8-8] | 8 [8-8] |
| Tabs | new tab -> NavigationCompleted (ms, median) | 56.1 [49.4-117.3] | 69.3 [63.6-96.8] | 111.0 [63.3-215.3] |
| Tabs | tab switch -> 2 frames (ms, median) | 19.6 [18.2-21.9] | 39.7 [38.7-60.4] | 45.3 [38.6-75.1] |
| Tabs | tab switch -> 2 frames (ms, p95) | 25.0 [21.2-40.1] | 51.3 [44.9-72.9] | 66.5 [46.0-81.8] |
| Tabs | wake sleeping tab -> 2 frames (ms, median) | 17.6 [15.7-22.5] | 122.0 [105.3-166.7] | 126.8 [99.8-283.5] |
| Tabs | destroy + recreate -> NavigationCompleted (ms, median) | 62.9 [57.0-73.2] | 68.0 [66.7-123.9] | 100.3 [67.2-198.1] |
| Tabs | destroy + recreate -> 2 frames (ms, median) | 70.8 [61.9-74.5] | 88.8 [88.4-156.3] | 127.1 [88.7-239.8] |
| Tabs | tabs asleep at 5 / at 10 | 4 / 9 | 4 / 9 | 4 / 9 |
| IPC | ping round trip (ms, mean of 1000) | 0.076 [0.065-0.131] | 1.102 [1.020-3.241] | 1.492 [1.066-8.309] |
| IPC | ping round trip (ms, p99) | 0.200 [0.200-0.300] | 1.400 [1.300-5.801] | 2.400 [1.300-28.602] |
| IPC | 1000 concurrent pings (calls/s) | 91743 [72464-100000] | 2534 [1406-2786] | 2154 [923-2623] |
| IPC | 1 MiB echo round trip (ms, median of 5) | 16.9 [15.1-20.5] | 81.6 [73.9-151.1] | 86.6 [81.0-189.2] |
| IPC | event emit, 10000 events (events/s) | 193424 [135318-202429] | 29744 [16708-34990] | 29595 [12127-36010] |
| Window | move: content follows after (ms, median) | 0.85 [0.52-1.37] | 50.92 [50.84-51.09] | 2.32 [1.67-2.97] |
| Window | move: content follows after (ms, p95) | 1.64 [0.76-1.95] | 51.53 [51.43-51.65] | 2.82 [1.82-3.98] |
| Window | 60 Hz drag: frames in sync (%) | 100 [100-100] | 2 [2-2] | 100 [37-100] |
| Window | 60 Hz drag: catch-up after last step (ms) | 0.47 [0.37-0.61] | 50.51 [50.35-50.62] | 1.71 [1.61-425.77] |
| Window | resize: content follows after (ms, median) | 3.31 [3.01-4.91] | 3.42 [1.98-4.35] | 3.22 [1.79-8.40] |
| Window | 60 Hz live resize: frames in sync (%) | 100 [100-100] | 100 [68-100] | 100 [70-100] |
| Window | 60 Hz live resize: catch-up (ms) | 2.90 [1.91-4.13] | 2.49 [2.00-7.39] | 2.47 [1.70-14.42] |
| Lifecycle | whole scenario (s) | 33.2 [32.9-34.4] | 39.4 [37.6-58.4] | 43.3 [36.5-66.0] |
| Lifecycle | shutdown (ms) | 500 [382-17579] | 205 [192-596] | 273 [192-644] |

### Sizes

| Binary | Bytes | ZIP (Deflate, optimal) |
|---|---:|---:|
| `boson_demo.exe` (Release, LTO, `/MD`, demo frontend embedded) | 1,337,344 | 531,229 |
| `boson_demo.exe` (Release, LTO, static CRT, `-DBOSON_STATIC_CRT=ON`) | 1,586,688 | 653,998 |
| `boson_browser.exe` (Release, LTO, `/MD`, with the benchmark driver) | 1,255,424 | 485,170 |
| `boson_browser.exe` (Release, LTO, static CRT) | 1,486,848 | 605,866 |
| `wails_browser.exe` (production, `-s -w`) | 10,578,432 | 4,292,581 |

- `/MD` builds need the Visual C++ 2015-2026 redistributable (`MSVCP140`, `MSVCP140_ATOMIC_WAIT`, `VCRUNTIME140`,
  `VCRUNTIME140_1`), which is often present but is not guaranteed on a clean Windows. The static-CRT build and the
  Wails binary depend only on Windows DLLs. The WebView2 runtime is assumed installed for both (Evergreen).
- `boson_browser.exe` was 1,094,144 bytes before the benchmark driver (`examples/browser/bench_driver.hpp`) was
  compiled into it; the driver costs ~156 KB.
- **Installer size was not measured**: neither NSIS (which `wails3 package` uses on Windows) nor WiX is installed on
  this machine, so no Wails installer could be built, and Boson's MSIX/ZIP packaging was not run for this comparison.
  The ZIP column is the closest proxy for a download.

### Reading the results

Where Boson is ahead:

- **Host process**: ~5-7 MB private bytes against ~52-67 MB for the Go runtime + Wails.
- **IPC**: ~15x lower ping latency, ~60x the concurrent call rate, ~6x faster 1 MiB echo, ~8x the event rate. Wails
  calls go through `fetch` to its asset server; Boson's go through `postMessage` and a reflected router.
- **Tab switch** (17.5 vs 44.5 ms) and especially **wake** (15 ms vs 122 ms): `TrySuspend` keeps the page, Wails has to
  recreate and reload it. The Wails tab switch also pays for showing a separate top-level window.
- **Move/drag**: Boson's content is a child window and moves with the frame (in sync every frame). Default Wails is
  ~51 ms behind on every move because of the 50 ms `WindowDidMove` debounce; with the debounce at 1 ms it is ~2.5 ms
  behind and in sync in 90-100 % of frames, still behind Boson.
- **Memory with tabs awake**: at 5 and 10 awake tabs the WebView2 tree is ~160-235 MB smaller with Boson (one environment,
  child WebViews vs one window + WebView per tab).
- **Startup**: the Boson window appears in ~32 ms (the native window is shown before WebView2 starts) vs ~250-310 ms;
  shell first paint and first tab navigation are ~110 ms earlier.

Where Wails is ahead or even:

- **Memory with 10 tabs asleep**: Wails' tree working set is ~55 MB lower (495 vs 552 MB) and whole-tree private WS is
  equal (220 MB), because a discarded tab's renderer process exits (8 processes left) while a suspended Boson tab
  keeps its renderer (17 processes). This is the price of a wake in 15 ms instead of a reload; an app that prefers
  memory can destroy hidden tabs in Boson too (that is the "destroy + recreate" row).
- **Resize**: the content follows a resize ~3 ms sooner in Wails (3.7 vs 6.9 ms median; ranges overlap). Boson lays the
  content out from `WM_SIZE` through the WebView2 controller's bounds; the Wails hook calls `SetWindowPos` directly.
  Live-resize sync is 100 % median for both.
- **Shutdown**: Wails' whole process tree is gone ~260 ms after `WM_CLOSE`, Boson's after ~560 ms (measured until every
  process of the tree has exited; not investigated further).
- **Destroy + recreate** with the default Wails build is in the same range as Boson (82.6 vs 68.1 ms to
  `NavigationCompleted`; the ranges overlap heavily).

## Boson vs Electron vs Tauri v2 vs Wails v3

The same small reference app is built in all four frameworks (`bench/compare/`) and driven by one harness
(`bench/run_compare.ps1`). Nothing here uses `boson_bench`: the harness is PowerShell plus a small C# process-tree
sampler (`bench/compare/harness/ProcTree.cs`, compiled with `Add-Type`) so that one command builds, runs and reports.

### What is compared

| | Boson | Electron | Tauri v2 | Wails v3 |
|---|---|---|---|---|
| Source | `bench/compare/boson/` (C++26, `boson_compare_app`) | `bench/compare/electron/` | `bench/compare/tauri/` | `bench/compare/wails/` |
| Version | this repository | Electron 45.5.0 | tauri 2.12.7 (Cargo.lock) | wails v3.1.0, Go 1.27 |
| Web engine | system WebView2 | bundled Chromium | system WebView2 | system WebView2 |
| Build | MSVC, Release, `/O2`, LTO, `/MD` | `@electron/packager`, asar | `cargo build --release --features custom-protocol`, `lto`, `opt-level = "s"`, `panic = "abort"`, strip | `-tags production -trimpath -ldflags "-w -s -H windowsgui"` |
| Frontend delivery | embedded in the exe, served from memory (`boson://app`) | asar, `file://` | embedded in the exe (`custom-protocol`) | embedded in the exe (asset server) |
| IPC | `postMessage` -> Bridge -> reflected `Router` on a worker pool | `ipcRenderer.invoke` (contextBridge, sandboxed renderer) | `invoke` (async commands, tokio) | `Call.ByName` (HTTP to the asset server) |
| Events | `Bridge::emit_to("main")` | `webContents.send` | `emit_to(label)` | `Event.Emit` (broadcast: Wails v3 has no per-window emit) |

The frontend is `bench/compare/shared/` (`index.html`, `style.css`, `app.js`: 48 static cards, byte-identical in all four);
each framework only adds a 15-line `adapter.js` that maps `invoke(name, arg)` / `listen(event, cb)` to its IPC. Every app has the
same commands: `config`, `ping(n)`, `echo(text)`, `items()` (a fresh array of 10 000 `{id, name, value, active}` on every call),
`burst(n)` (emits n events carrying the index), `open_window()` (second window, same page with `?w=2`), `mark(text)` and
`report(json)` (the last two write files into `COMPARE_DIR` for the harness). Window: 900x640, standard frame, default settings.

### Method

One run = one process tree, launched with `COMPARE_DIR` (marks and results go there) and `COMPARE_CONFIG`. The page drives its own
timeline and the harness only watches marks and samples the tree, so nothing polls in the app while it idles:

1. **Startup** (wall clock from just before `CreateProcess`): first visible, titled, >= 100x100 px top-level window of the launched
   process (EnumWindows, 1 ms polling, no process snapshots); **first paint** = page-side timestamp of the second
   `requestAnimationFrame` after `DOMContentLoaded`; **page ready** = first completed IPC round trip (`ping`). Page timestamps are
   `performance.timeOrigin + performance.now()`, the same system clock as the harness.
2. **Idle, 1 window**: 3 s settle, then 30 s of nothing (the page sleeps in one `setTimeout`). Reported: CPU time of every process that
   ever belonged to the tree (handles are kept, so processes that exit still count) as % of one core and as Mcycles/s
   (`QueryProcessCycleTime`), context switches/s (sum of thread counters from `NtQuerySystemInformation`; threads that exit
   in the window take their count with them), and the system-wide busy % of the same window as a noise indicator.
3. **Memory, 1 window**: median of 5 samples, 400 ms apart, right after the idle window: host (launched) process private bytes and
   private working set, whole tree private working set (what Task Manager shows as "Memory"), whole tree commit, process /
   thread / handle counts. The tree is found by a Toolhelp parent-pid walk with a creation-time veto against pid reuse.
4. **Second window**: `open_window()`; time until its page completes an IPC round trip, then the same memory sampling after 2 s.
5. **IPC** (all measured in the page with `performance.now()`): 200 warm-up pings; 2000 sequential pings (mean = wall time of
   the loop / calls, p99 from per-call timings, which Chromium quantises to 100 us); 1000 concurrent pings (calls/s); echo of 1 KiB
   (500 calls), 64 KiB (200) and 1 MiB (30) of pseudo-random text, each reply verified byte for byte; the 10k-item array (30
   calls, shape verified); five bursts of 10 000 events counted in the page (order checked, 15 s timeout).
6. **Shutdown**: `WM_CLOSE` to the tree's windows; time until the launched process has exited, and until every process of the
   tree is gone (waits on process handles; no polling, because polling the process list during teardown was seen to delay the
   WebView2 browser process).

5 measured runs after 1 warm-up run per framework, interleaved with a rotating starting framework (round 1: boson, electron, tauri, wails;
round 2: electron, tauri, wails, boson; ...), 3 s cooldown. The WebView2 user-data folders and the Electron profile are warm (persist
between runs). The harness makes the app windows topmost without activating them: on a shared machine another window on top of an
app made Chromium treat it as occluded (rAF paused, renderer at idle priority) and stalled runs for minutes.

Re-run everything (builds all four apps, ~10-15 min the first time, then about 40 min of runs):

```powershell
powershell -ExecutionPolicy Bypass -File bench\run_compare.ps1                       # -Runs 5 -Warmup 1 -IdleSeconds 30 by default
powershell -ExecutionPolicy Bypass -File bench\run_compare.ps1 -SkipBuild            # reuse build\rel and the other apps' outputs
powershell -ExecutionPolicy Bypass -File bench\run_compare.ps1 -Frameworks boson,tauri -Runs 3
```

Needs Node + npm (Electron, downloaded by the packager), Rust/cargo (Tauri), Go + `wails3` (Wails) and the MSVC toolset (Boson);
pass `-CMakeArgs` for `FETCHCONTENT_SOURCE_DIR_*` when the Boson dependencies are not fetched. Outputs land in
`build\bench-results\compare\` (`comparison.md`, `sizes.md`, `results.json` with every run, `runs\<fw>-<n>\` with the marks and
each app's result). The 2026-11-26 baseline is kept in `bench/compare/results/2026-11-26/`. Individual apps:
`bench\compare\{electron,tauri,wails}\build.ps1`, `cmake --build build\rel --target boson_compare_app`.

### Results (measured 2026-11-26)

- Windows 11 Pro 10.0.26300 (26H2), Intel Core i7-13700KF (24 logical CPUs), 48 GB RAM, WebView2 runtime 158.0.4370.33.
- **Not a quiet machine**: other agents were building and running UI tests during the whole run (system-wide CPU busy 20-45 % in
  the idle windows, see the noise row). Medians are robust to that, the min-max ranges show it; all 20 measured runs completed.
- Boson at commit 71c3757 plus the reference app; Release + LTO, `/MD` (needs the VC++ redistributable, which is not part of the size).
- Cells are median [min-max] over 5 runs; the best median of a row is bold. Ties inside the noise are not claims.

| Group | Metric | boson | electron | tauri | wails |
|---|---|---:|---:|---:|---:|
| Startup | first visible window (ms) | 29 [26-43] | 107 [104-121] | **29 [28-43]** | 381 [306-641] |
| Startup | first paint (ms) | 312 [273-366] | **202 [197-364]** | 373 [328-400] | 396 [307-658] |
| Startup | page ready: first IPC round trip (ms) | 314 [274-367] | **204 [198-365]** | 378 [333-408] | 403 [312-672] |
| Idle 30 s, 1 window | tree CPU (% of one core) | **0.10 [0.05-2.40]** | 1.77 [0.10-5.31] | 0.21 [0.10-2.14] | 1.67 [0.10-5.63] |
| Idle 30 s, 1 window | tree CPU (Mcycles/s) | **9.1 [7.3-88.9]** | 81.8 [9.9-187.7] | 10.7 [9.3-75.8] | 56.0 [12.6-195.1] |
| Idle 30 s, 1 window | tree context switches (/s) | 11 [4-527] | 1,125 [97-2,371] | **10 [7-458]** | 411 [47-1,551] |
| Idle 30 s, 1 window | system-wide CPU busy during the window (%, noise indicator) | 30.7 [20.3-45.4] | 34.0 [30.3-40.6] | 39.0 [31.4-42.5] | 37.3 [33.0-45.4] |
| Memory, 1 window | host process private bytes (MB) | **4.7 [4.6-4.9]** | 31.4 [31.0-33.2] | 5.6 [5.5-5.7] | 51.6 [51.5-54.9] |
| Memory, 1 window | host process private working set (MB) | **3.4 [3.3-3.5]** | 23.5 [23.2-24.2] | 4.0 [3.9-4.0] | 6.6 [6.4-9.1] |
| Memory, 1 window | whole tree private working set (MB) | 92.3 [87.7-92.4] | **91.2 [88.4-94.3]** | 92.3 [90.9-94.1] | 96.2 [94.4-102.8] |
| Memory, 1 window | whole tree private bytes / commit (MB) | 162.2 [157.8-162.6] | **149.9 [146.9-154.1]** | 163.4 [160.8-166.7] | 212.4 [209.9-218.3] |
| Memory, 1 window | processes | 7 [7-7] | **4 [4-4]** | 7 [7-7] | 7 [7-7] |
| Memory, 1 window | threads | 207 [201-210] | **120 [118-123]** | 209 [204-212] | 195 [192-199] |
| Memory, 1 window | handles | 3,904 [3,877-3,906] | **2,805 [2,796-2,828]** | 3,963 [3,933-3,970] | 4,039 [4,000-4,061] |
| Memory, 2 windows | second window: request -> its IPC ready (ms) | 101 [99-105] | **89 [75-110]** | 132 [129-222] | 150 [141-748] |
| Memory, 2 windows | host process private bytes (MB) | **4.9 [4.9-5.0]** | 33.4 [32.2-34.2] | 6.0 [5.8-6.0] | 52.5 [52.3-55.5] |
| Memory, 2 windows | whole tree private working set (MB) | **107.3 [102.7-107.6]** | 117.1 [115.2-120.7] | 110.0 [109.9-113.4] | 114.9 [111.7-120.2] |
| Memory, 2 windows | whole tree private bytes / commit (MB) | 200.0 [197.8-202.4] | **195.4 [192.6-201.4]** | 212.7 [210.8-214.9] | 255.5 [250.6-259.1] |
| Memory, 2 windows | processes | 8 [8-8] | **5 [5-5]** | 8 [8-8] | 8 [8-8] |
| IPC | ping, sequential: mean (ms) | 0.128 [0.078-0.394] | **0.112 [0.073-0.127]** | 1.507 [0.999-1.817] | 2.272 [1.122-3.041] |
| IPC | ping, sequential: p99 (ms) | **0.30 [0.20-1.50]** | 0.30 [0.20-0.40] | 2.60 [2.00-3.90] | 4.30 [2.20-7.50] |
| IPC | ping, 1000 concurrent (calls/s) | 64,935 [39,841-92,593] | **95,238 [50,761-102,041]** | 3,019 [2,396-4,257] | 1,679 [1,337-2,061] |
| IPC | echo 1 KiB: mean (ms) | 0.143 [0.093-0.210] | **0.129 [0.075-0.155]** | 1.436 [1.150-1.833] | 2.327 [1.366-3.515] |
| IPC | echo 1 KiB: p99 (ms) | **0.30 [0.20-0.40]** | 0.30 [0.20-0.60] | 2.50 [2.00-3.10] | 3.40 [2.60-6.20] |
| IPC | echo 64 KiB: mean (ms) | 1.873 [1.482-2.320] | **0.480 [0.334-0.518]** | 4.376 [3.079-5.211] | 12.524 [7.950-14.678] |
| IPC | echo 64 KiB: p99 (ms) | 2.70 [2.10-3.80] | **1.32 [1.00-2.50]** | 7.37 [4.90-9.14] | 16.60 [10.60-18.40] |
| IPC | echo 1 MiB: mean (ms) | 26.38 [20.48-31.37] | **2.85 [2.54-4.62]** | 36.04 [29.84-41.30] | 138.16 [97.16-156.74] |
| IPC | echo 1 MiB: p99 (ms) | 29.6 [22.2-34.1] | **6.4 [4.3-16.3]** | 40.6 [32.6-48.9] | 156.4 [117.6-361.9] |
| IPC | echo 1 MiB: payload throughput (MiB/s, one way) | 37.9 [31.9-48.8] | **350.9 [216.6-393.7]** | 27.7 [24.2-33.5] | 7.2 [6.4-10.3] |
| IPC | 10k-item JSON array: mean (ms) | 17.49 [13.00-18.82] | **10.11 [9.50-20.34]** | 11.72 [9.21-12.79] | 17.91 [11.08-19.07] |
| IPC | 10k-item JSON array: p99 (ms) | 22.2 [14.6-25.2] | 16.2 [13.6-26.3] | **14.5 [10.9-15.4]** | 22.0 [13.8-22.9] |
| Events | backend -> page burst of 10000 (events/s, median of 5 bursts) | 107,527 [91,408-167,785] | **167,224 [146,843-187,970]** | 23,798 [20,096-33,557] | 14,455 [13,976-25,432] |
| Lifecycle | shutdown: WM_CLOSE -> app process exited (ms) | 308 [273-468] | 80 [79-115] | 60 [49-113] | **57 [32-77]** |
| Lifecycle | shutdown: WM_CLOSE -> whole tree gone, incl. WebView2 browser process (ms; capped at 60000) | 308 [273-468] | **81 [79-115]** | 126 [114-297] | 136 [95-190] |

### Sizes

| Framework | Main executable (bytes) | Distribution folder (bytes) | Files | ZIP, Deflate optimal (bytes) |
|---|---:|---:|---:|---:|
| boson | 813 056 | 813 056 | 1 | 317 671 |
| electron | 250 417 664 | 374 629 434 | 73 | 158 448 683 |
| tauri | 3 228 160 | 3 228 160 | 1 | 1 315 698 |
| wails | 10 533 888 | 10 533 888 | 1 | 4 271 275 |

- Boson, Tauri and Wails use the WebView2 runtime that ships with Windows 11 / Edge; it is **not** in these sizes (Electron carries its own
  Chromium, 375 MB in 73 files). The ZIP column is the closest proxy for a download. **No installers were built** (NSIS / WiX / MSIX
  were not run for any of the four); do not read the ZIP as an installer size.
- Boson's 813 KB is the `/MD` build; the static-CRT build is ~20 % larger and has no redistributable dependency. Tauri's
  3.2 MB is `opt-level = "s"` with the default feature set; Wails' 10.5 MB is the Go runtime and its dependencies.

### Reading the results

Where Boson is ahead (outside the noise):

- **Host process memory**: 4.7 MB private bytes against 5.6 (Tauri), 31 (Electron main) and 52 (Wails: Go runtime).
- **Idle CPU**: median 0.10 % of one core over 30 s, 9 Mcycles/s; Electron and Wails use ~17x the CPU time (1.7-1.8 %) and 6-9x the cycles (56-82 Mcycles/s) and
  switch contexts 40-100x more often (Electron 1125/s, Wails 411/s, Boson 11/s). Tauri is close (0.21 %, 10 Mcycles/s, 10 switches/s):
  the two WebView2 hosts behave alike, the spread inside one framework (0.05-2.4 %) is larger than their difference.
- **First window**: 29 ms (the native window shows before WebView2 starts), tied with Tauri; Electron 107 ms, Wails 381 ms.
- **Binary**: 0.8 MB / 0.3 MB zipped, 4x smaller than Tauri, 13x smaller than Wails, ~460x smaller than the Electron folder.
- **IPC against the other two WebView2 hosts**: small-call latency 12-18x lower than Tauri and Wails (0.13 ms vs 1.5 / 2.3 ms), ~20-40x the
  concurrent call rate, ~4.5-7x the event rate (108k vs 24k / 14k events/s), and 1 MiB echo 1.4x / 5x faster.
- **Two windows**: lowest whole-tree private working set (107 MB vs 110-117).

Where Boson is **not** ahead:

- **Large payloads and bulk data vs Electron**: Electron's in-process-style Mojo IPC wins clearly. 64 KiB echo 0.48 ms vs 1.87 ms (3.9x), 1 MiB echo
  2.85 ms vs 26.4 ms (9x; 351 vs 38 MiB/s one way), 10k-item array 10.1 ms vs 17.5 ms (1.7x), event burst 167k/s vs 108k/s (1.6x). WebView2's
  `postMessage` crosses the browser/renderer process boundary and the page parses JSON strings; this is the biggest gap in the table.
- **Small-call latency vs Electron**: effectively tied (0.128 ms vs 0.112 ms mean, identical p99 0.30 ms; Boson's range 0.08-0.39 ms is wider);
  Electron is ahead on concurrent pings (95k vs 65k calls/s).
- **First paint**: Electron paints at 202 ms, Boson at 312 ms (Tauri 373, Wails 396). All three WebView2 hosts wait for the runtime's
  browser process and renderer; Chromium in-process startup is simply faster here.
- **Whole-tree memory is a tie** (private working set 91-92 MB for Electron, Boson and Tauri, Wails 96): the renderer/GPU/network/storage
  processes of WebView2 (or Electron) dominate and are not under the host's control. Commit (private bytes) is lowest for Electron (150 MB vs
  162 MB), which also runs 4 processes against 7; Boson, Tauri and Wails are within 2 MB of each other.
- **Shutdown**: Boson's process needs 308 ms from `WM_CLOSE` to exit; the others 57-80 ms. Electron's whole tree is gone in 81 ms; the WebView2 hosts
  then wait for the runtime's browser process for another ~65-80 ms (whole tree: Tauri 126 ms, Wails 136 ms; for Boson both numbers are 308 ms).
- **Tauri 10k-item array p99** (14.5 ms vs 22.2 ms) and mean (11.7 vs 17.5 ms) are better than Boson's; Electron is best on the mean.

### Caveats

- The WebView2 browser process sometimes outlives its host by 10-60 s on this machine (seen with Boson, Tauri and Wails while developing
  the harness, mostly after earlier runs were killed or while the machine was saturated); the harness waits up to 60 s and never kills in the normal case, and a killed run
  is followed by a 30 s pause because a killed browser leaves the profile dirty (the next start of that app then takes ~20 s). None of the 20 measured runs needed a kill or a pause: every shutdown in the table finished by itself within 0.5 s. Individual outliers (e.g. 17-60 s) are possible and would
  show up in the max of the range.
- Idle CPU and context switches are dominated by Chromium housekeeping that comes and goes (the ranges are wide); with 5 runs the median
  is stable to a few hundredths of a percent, no more. Cycle counts per second are the less noisy number.
- A page's timer / rAF / IPC speed in the second window was not compared (only that it comes up and answers).
- The comparison is "default configuration, production build". Electron could be trimmed (fewer locales, no GPU process, ...), Tauri and
  Wails have their own tuning knobs, Boson has none applied here; none of that was tried.
- Windows power plan, AC power and display were whatever the workstation had; the app windows were topmost but not focused.

## Verification

- The harness core (tree walk with PID-reuse veto, job enumeration, statistics, phase summaries, markdown and
  comparison rendering, JSON round trip) is unit-tested in `boson_porting_tests`; live sampling is tested on the test
  process and a real `cmd.exe -> ping.exe` tree inside a job (CPU accounting after exit included).
- All numbers above come from real runs of `boson_bench` on this machine (reports were written to
  `build/bench-results/` and the session scratchpad, which are not committed). Nothing was measured for Electron or Tauri.
- The scenario core (checkpoint and HWND protocol parsing, per-checkpoint memory reduction and medians, the app
  result JSON as the Go side writes it, metric rows with warm-up and failed runs excluded, markdown and comparison
  rendering, JSON round trip) and the window-lag probe (a child window that follows its parent: no timeouts, 100 %
  in sync; an unrelated window: every probe times out) are unit-tested in `boson_porting_tests`.
- The SleepPolicy fix found while building the scenario (a tab woken and hidden again before the next sleep check
  stayed marked "sleeping" and was never suspended again) is covered in `boson_integration_tests`.
- The Release + LTO build (`build\rel`, `build\rel-mt`) compiled and linked with no warnings or errors;
  the Release startup/idle numbers in the first table were **not** re-measured (they are still the Debug numbers).
- The Wails app was driven end-to-end through the harness (all 12 runs of both Wails configurations completed). Not verified: the browser scenario
  on Windows 10, on a quiet machine, at a DPI other than this machine's, and installer sizes (no NSIS/WiX).
- The window probe measures window geometry, not presentation: a content HWND in place does not prove its pixels
  were composed in the same frame.
- **Comparison suite (bench/compare, run_compare.ps1)**: all four apps were built and driven end to end by the harness (5 measured runs +
  1 warm-up each, every run completed, the IPC replies were verified for content). The Boson app builds warning-free (/W4) in Debug (`build\wt`)
  and Release + LTO; `boson_compare_harness_smoke` (ctest, Debug, ~55 s) runs the harness against the Boson app and passes. The harness
  was developed against the failure modes described under "Caveats" (occluded windows, killed WebView2 profiles, polling that perturbed
  shutdown); its process-tree sampler has no unit tests of its own beyond that smoke test.
- **Not verified for the comparison**: a quiet machine (other agents were working), Windows 10, other DPIs / monitors, battery power,
  installers (none built), the static-CRT Boson build in the comparison, Electron / Tauri / Wails with their tuning options, a cold
  (first-ever) WebView2 profile, and anything other than the one workstation. The per-process context-switch figure undercounts threads
  that exit during the window. The page-side latency timer is quantised to 100 us.
