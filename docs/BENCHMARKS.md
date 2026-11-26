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
- **Noisy machine**: other agents were compiling and running tests at the same time. The spread columns show it;
  read the medians, and treat single-digit-ms differences as noise.

| Group | Metric | boson | wails | wails_debounce1 |
|---|---|---:|---:|---:|
| Startup | first window (ms) | 32 [22-50] | 314 [228-408] | 257 [198-430] |
| Startup | shell first paint (ms) | 279 [229-428] | 396 [314-615] | 382 [275-572] |
| Startup | first tab NavigationCompleted (ms) | 239 [194-370] | 347 [279-540] | 345 [234-513] |
| Memory tabs_1 | host private bytes (MB) | 4.7 [4.7-4.8] | 52.1 [52.1-53.6] | 53.1 [52.3-53.5] |
| Memory tabs_1 | WebView2 tree working set (MB) | 358.6 [355.5-360.4] | 375.2 [373.5-380.6] | 376.7 [375.0-379.4] |
| Memory tabs_1 | whole tree private WS (MB) | 99.5 [96.6-101.5] | 106.2 [105.6-111.8] | 106.9 [106.3-110.7] |
| Memory tabs_1 | processes | 8 [8-8] | 8 [8-8] | 8 [8-8] |
| Memory tabs_5 | host private bytes (MB) | 7.1 [6.1-7.6] | 67.0 [65.6-68.7] | 65.4 [63.8-68.1] |
| Memory tabs_5 | WebView2 tree working set (MB) | 648.1 [644.0-689.4] | 811.3 [807.5-812.9] | 806.6 [754.1-810.2] |
| Memory tabs_5 | whole tree private WS (MB) | 221.4 [218.4-258.4] | 365.6 [360.7-369.2] | 360.6 [295.1-363.7] |
| Memory tabs_5 | processes | 12 [12-13] | 12 [12-12] | 12 [12-12] |
| Memory tabs_5_slept | host private bytes (MB) | 7.1 [6.1-7.6] | 65.8 [64.7-67.3] | 64.4 [62.6-67.1] |
| Memory tabs_5_slept | WebView2 tree working set (MB) | 482.6 [481.7-524.0] | 528.0 [490.4-529.3] | 526.2 [488.6-530.4] |
| Memory tabs_5_slept | whole tree private WS (MB) | 187.3 [183.8-223.2] | 262.1 [211.3-264.8] | 260.7 [210.3-262.1] |
| Memory tabs_5_slept | processes | 12 [12-13] | 8 [8-8] | 8 [8-8] |
| Memory tabs_10 | host private bytes (MB) | 7.4 [6.5-7.9] | 66.8 [65.2-68.6] | 66.2 [63.5-68.4] |
| Memory tabs_10 | WebView2 tree working set (MB) | 798.1 [795.9-839.0] | 1033.2 [1029.1-1060.9] | 1067.5 [1025.3-1076.2] |
| Memory tabs_10 | whole tree private WS (MB) | 266.3 [262.2-302.4] | 348.0 [346.5-389.8] | 395.7 [340.9-401.9] |
| Memory tabs_10 | processes | 17 [17-18] | 17 [17-17] | 17 [17-17] |
| Memory tabs_10_slept | host private bytes (MB) | 7.4 [6.5-7.9] | 66.0 [64.7-67.6] | 65.3 [63.0-67.9] |
| Memory tabs_10_slept | WebView2 tree working set (MB) | 551.9 [546.5-599.3] | 495.3 [494.6-498.4] | 495.2 [492.3-533.0] |
| Memory tabs_10_slept | whole tree private WS (MB) | 220.4 [218.7-265.3] | 220.0 [216.5-224.5] | 220.6 [214.1-264.7] |
| Memory tabs_10_slept | processes | 17 [17-18] | 8 [8-8] | 8 [8-8] |
| Tabs | new tab -> NavigationCompleted (ms, median) | 51.4 [47.8-104.9] | 77.9 [62.8-134.2] | 66.3 [62.9-140.0] |
| Tabs | tab switch -> 2 frames (ms, median) | 17.5 [14.6-30.1] | 44.5 [42.4-90.0] | 39.2 [35.5-61.5] |
| Tabs | tab switch -> 2 frames (ms, p95) | 21.1 [18.6-68.1] | 54.2 [50.2-104.8] | 66.7 [39.6-73.1] |
| Tabs | wake sleeping tab -> 2 frames (ms, median) | 15.2 [13.4-23.6] | 121.8 [105.3-184.3] | 143.6 [93.7-200.7] |
| Tabs | destroy + recreate -> NavigationCompleted (ms, median) | 68.1 [47.5-163.4] | 82.6 [67.6-143.8] | 161.1 [68.8-401.6] |
| Tabs | destroy + recreate -> 2 frames (ms, median) | 73.4 [51.8-176.4] | 105.1 [88.5-182.1] | 199.5 [88.9-460.6] |
| Tabs | tabs asleep at 5 / at 10 | 4 / 9 | 4 / 9 | 4 / 9 |
| IPC | ping round trip (ms, mean of 1000) | 0.151 [0.059-0.224] | 2.333 [2.058-5.397] | 2.323 [1.072-4.324] |
| IPC | ping round trip (ms, p99) | 0.300 [0.200-0.500] | 5.103 [3.301-29.419] | 6.200 [1.500-38.510] |
| IPC | 1000 concurrent pings (calls/s) | 75188 [33557-106383] | 1217 [698-1609] | 1203 [902-2556] |
| IPC | 1 MiB echo round trip (ms, median of 5) | 21.9 [14.1-32.7] | 139.5 [109.2-265.8] | 147.2 [74.7-249.9] |
| IPC | event emit, 10000 events (events/s) | 131234 [74571-194553] | 16359 [9350-24624] | 15908 [10089-27159] |
| Window | move: content follows after (ms, median) | 0.94 [0.50-1.07] | 51.51 [50.52-52.32] | 2.54 [1.71-7.09] |
| Window | move: content follows after (ms, p95) | 1.67 [0.73-2.57] | 53.86 [51.13-76.09] | 3.40 [1.85-21.37] |
| Window | 60 Hz drag: frames in sync (%) | 100 [100-100] | 2 [2-2] | 100 [90-100] |
| Window | 60 Hz drag: catch-up after last step (ms) | 0.68 [0.28-0.93] | 50.56 [50.37-52.40] | 1.69 [1.19-2.03] |
| Window | resize: content follows after (ms, median) | 6.91 [3.48-7.85] | 3.73 [2.34-13.44] | 4.98 [2.30-13.49] |
| Window | 60 Hz live resize: frames in sync (%) | 100 [100-100] | 100 [30-100] | 100 [68-100] |
| Window | 60 Hz live resize: catch-up (ms) | 3.51 [2.18-8.04] | 3.40 [2.28-17.80] | 2.46 [1.44-7.79] |
| Lifecycle | whole scenario (s) | 32.9 [32.8-35.1] | 41.4 [39.8-48.6] | 40.8 [38.2-151.2] |
| Lifecycle | shutdown (ms) | 563 [449-1352] | 257 [194-368] | 252 [192-1410] |

### Sizes

| Binary | Bytes | ZIP (Deflate, optimal) |
|---|---:|---:|
| `boson_demo.exe` (Release, LTO, `/MD`, demo frontend embedded) | 1,175,552 | 465,532 |
| `boson_demo.exe` (Release, LTO, static CRT, `-DBOSON_STATIC_CRT=ON`) | 1,422,848 | 589,967 |
| `boson_browser.exe` (Release, LTO, `/MD`, with the benchmark driver) | 1,250,304 | 484,305 |
| `boson_browser.exe` (Release, LTO, static CRT) | 1,481,216 | 605,084 |
| `wails_browser.exe` (production, `-s -w`) | 10,577,408 | 4,292,431 |

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
