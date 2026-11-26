# Performance

Findings of the optimization pass, one self-contained section per area. Raw numbers and the harnesses are described in
[BENCHMARKS.md](BENCHMARKS.md).

## Comparative benchmark suite

**Area**: `bench/compare/` + `bench/run_compare.ps1` (Boson vs Electron vs Tauri v2 vs Wails v3). This section is the baseline other
areas are measured against; no Boson code was changed for it (the only product-tree change is the `boson_compare_app` target and the
`boson_compare_harness_smoke` test in CMakeLists.txt).

### Baseline (2026-11-26, Windows 11, i7-13700KF, shared and busy machine, 5 runs + 1 warm-up, medians)

| Metric | Boson | Electron 45.5 | Tauri 2.12 | Wails 3.1 | Best |
|---|---:|---:|---:|---:|---|
| First visible window (ms) | 29 | 107 | 29 | 381 | Boson = Tauri |
| First paint (ms) | 312 | **202** | 373 | 396 | Electron |
| Idle CPU, 30 s (% of one core) | **0.10** | 1.77 | 0.21 | 1.67 | Boson |
| Idle context switches (/s) | 11 | 1125 | 10 | 411 | Boson = Tauri |
| Host process private bytes (MB) | **4.7** | 31.4 | 5.6 | 51.6 | Boson |
| Whole tree private WS, 1 window (MB) | 92.3 | 91.2 | 92.3 | 96.2 | tie |
| Whole tree private WS, 2 windows (MB) | **107.3** | 117.1 | 110.0 | 114.9 | Boson |
| Ping mean (ms) / concurrent (calls/s) | 0.128 / 64.9k | 0.112 / 95.2k | 1.51 / 3.0k | 2.27 / 1.7k | Electron, Boson close |
| Echo 64 KiB / 1 MiB mean (ms) | 1.87 / 26.4 | **0.48 / 2.85** | 4.38 / 36.0 | 12.5 / 138 | Electron |
| 10k-item array mean (ms) | 17.5 | **10.1** | 11.7 | 17.9 | Electron |
| Event burst (events/s) | 107k | **167k** | 24k | 14k | Electron |
| Shutdown, WM_CLOSE -> process exit (ms) | 308 | 80 | 60 | **57** | Wails |
| Executable / ZIP (KB) | **813 / 318** | 250 MB exe, 375 MB folder / 158 MB | 3,228 / 1,316 | 10,534 / 4,271 | Boson |

### Where the other areas have room (found by the comparison, not fixed here)

1. **Bulk IPC payloads**: Boson is 4x (64 KiB), 9x (1 MiB) behind Electron and about 1.4x ahead of Tauri; the 10k-item array and event burst
   are 1.7x / 1.6x behind Electron. Everything that crosses `postMessage` is a JSON string that WebView2 copies between the browser and
   renderer processes and the page parses. Candidates: `ICoreWebView2SharedBuffer` / `PostSharedBufferToScript` for payloads above a
   threshold, `PostWebMessageAsString` without a JSON envelope for string replies, batching events into one message per frame.
2. **First paint** (312 ms vs Electron's 202 ms): the window is up in 29 ms, the remaining ~280 ms is environment creation, controller creation
   and navigation. Candidates: create the environment on a background thread while the window is created, avoid a second navigation, check
   the `boson://` filter registration order.
3. **Shutdown** (308 ms vs 57-80 ms): not investigated; during development runs with a slow WebView2 teardown the log showed
   `Environment teardown: BrowserProcessExited not observed` (HRESULT 0x800705B4) after the 3 s timeout; whether that wait costs time in the
   normal 300 ms case is unknown.
4. **Idle**: already the lowest of the four; the spread inside a framework (0.05-2.4 %) is larger than the gap to Tauri. Do not trade the
   zero-idle-wakeup design for it.

### Notes for whoever re-runs this

- Run it on a machine with nothing else running (see the noise row in the results); `system_busy_pct` in results.json tells how noisy a run was.
- Compare against the committed baseline `bench/compare/results/2026-11-26/results.json` (every run, every metric); `-Frameworks boson`
  re-measures only Boson (compare its column with the baseline by hand), `-Runs 5` is the minimum for medians.
- The harness raises the app windows to topmost (without activation) and keeps all four apps on identical terms; if a framework's numbers
  change after an update, first check the "Caveats" list in BENCHMARKS.md.
- A change in `bench/compare/shared/` changes all four apps; rebuild all of them (`run_compare.ps1` without `-SkipBuild`) before comparing.
