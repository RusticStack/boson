# Benchmarks

Two pieces live in this repository:

- **`boson_bench`** (tools/bench/) — a framework-agnostic harness. It launches any executable, follows its whole
  process tree (Toolhelp parent-PID walk, unioned with a job object it creates around the target, so
  `msedgewebview2.exe`, Electron renderer/GPU/utility processes and crashpad helpers are all included, even when a
  child breaks away from the job), and samples private working set, working set, commit (private bytes),
  handle count and CPU time of the whole tree.
- **`boson_bench_app`** (examples/bench_app/) — a minimal Boson app with an in-page IPC load test.

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

## Running the same harness against Electron, Tauri and Wails

No Electron or Tauri toolchain is installed on this machine, so **no numbers are given for them** (Wails v3 is covered by the browser scenario above). To compare the generic startup/idle harness:

1. Build an equivalent app in each framework: one ~900x640 window, the same static page
   (copy `examples/bench_app/bench_page.hpp`'s HTML; replace `chrome.webview.postMessage` with the framework's IPC if
   you also want the IPC numbers) and an echo/ping handler. Build in **release** mode; for a fair comparison also build
   Boson in Release (`cmake -DCMAKE_BUILD_TYPE=Release`).
2. Run each with identical flags, e.g.
```bat
boson_bench --exe "C:\path\electron-bench\out\electron-bench-win32-x64\electron-bench.exe" --name electron --runs 3 --idle 30 --no-samples --out bench-results
boson_bench --exe "C:\path\tauri-bench\src-tauri\target\release\tauri-bench.exe" --name tauri --runs 3 --idle 30 --no-samples --out bench-results
boson_bench --exe "C:\path\wails-bench\bin\wails-bench.exe" --name wails --runs 3 --idle 30 --no-samples --out bench-results
boson_bench --compare bench-results\boson_bench_app.json bench-results\electron.json bench-results\tauri.json bench-results\wails.json --out bench-results\comparison.md
```
   For an unpackaged Electron app use `--exe node_modules\electron\dist\electron.exe -- .` from the app folder.
3. Optional ready signal: have the app write `QueryPerformanceCounter` into `%BOSON_BENCH_DIR%\ready` when its first frame
   is shown (Tauri: `windows` crate `QueryPerformanceCounter`; Wails/Go: `golang.org/x/sys/windows`; Electron main
   process: a tiny native addon or skip it) and add `--wait-ready`. Without it, compare "First window" only.
4. Optional load: implement the `BosonBench.StartLoad` / `load_done` protocol, or drive the app externally with
   `--load-command`.
5. Close other heavy programs, run on AC power, and report the machine line from the generated markdown with the numbers.

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

## IPC hot path (optimisation pass)

Native allocation / wake-up counts, per-call cost and end-to-end numbers for the IPC path (Router, Bridge, TaskRunner,
UiMarshal, UTF-8/UTF-16 conversion, event batching) are in [PERFORMANCE.md](PERFORMANCE.md#ipc-hot-path). Two harnesses were
added for it: `boson_ipc_bench` (`tests/bench_ipc.cpp`, counts allocations/bytes/wake-ups per call, no WebView needed) and an
`events_20k` scenario plus `--batch-events` flag in `boson_bench_app`. Summary on this machine (Release, medians of 5
interleaved runs): ping allocations 10 -> 3 per round trip, response bytes 1136 -> 64, UI wake-ups per call under load 1 -> 0.002,
1 MiB echo native path 4.1 -> 2.1 ms, page-counted events 150 k/s -> 161 k/s (606 k/s with opt-in batching), 10 MiB echo
275 -> 240 ms; 1000-ping latency is unchanged within noise. The headline numbers in the table above were not re-measured.
