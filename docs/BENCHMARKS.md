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

## Running the same harness against Electron, Tauri and Wails

No Electron, Tauri or Wails toolchain is installed on this machine, so **no numbers are given for them**. To compare:

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
  `build/bench-results/`, which is not committed). Nothing was measured for Electron, Tauri or Wails.
- Not verified: Release-build numbers, behaviour on Windows 10, and the harness against non-Boson targets (only
  Boson apps, `cmd.exe` and `ping.exe` were launched).
