# Performance

Each area of the optimisation pass has its own section. Method everywhere: measure before changing, change one thing,
measure after, interleave A/B runs, keep only what wins (or is neutral and simpler). Release + LTO builds
(`build\rel`), Windows 11 26H2, i7-13700KF, WebView2 runtime 158.0.4370.33, warm start (runtime and binaries in the
file cache). Other agents were building on the same machine for most of the session (CPU was at 100 % for hours),
so every number below says what the load was; wall-clock under load is only used for A/B pairs that ran interleaved.

## Startup, shutdown and lifecycle

### Tracing: `BOSON_TRACE`

`include/boson/core/trace.hpp`. `boson::trace::mark("name")` stores a QueryPerformanceCounter value in a fixed table
(1024 entries, lock-free, no allocation, any thread). With `BOSON_TRACE` unset it is one load of a plain `bool` and a
not-taken branch, no call. With it set, the table is written at process exit (atexit, or `trace::flush()`, which
`app::exit_process` calls) with times relative to **process creation** (`GetProcessTimes`):

```
set BOSON_TRACE=1                 -> OutputDebugString + stderr
set BOSON_TRACE=C:\temp\boson.log -> appended to that file (one block per process)
[boson-trace pid=18628 tid=26200]    25.300  +   0.900  webview.controller.call
```

Library markers: `window.create.{begin,styled,shown}`, `scheduler.pool.{begin,end}`, `env.create.{call,returned}`,
`env.created`, `webview.controller.{call,created}`, `webview.{configured,events_registered,navigate,nav_starting,
nav_completed}`, `asset.{request,looked_up,stream_and_headers,response_created,response,warm_up.*}`, and for the end
`window.wm_close`, `quit.begin`, `env.teardown.{begin,end}`, `env.browser_process_exited`, `quit.post_quit_message`,
`message_loop.exit`, `exit_process`, `trace.flush`. Apps add their own (the bench apps mark `page.first_frame`).

### Where the time goes (boson_bench_app, 8 runs, quiet machine, medians, ms since process creation)

| Marker | ms | Step |
|---|---:|---|
| `trace.init` (first static initializer of the exe) | 10.8 | process creation, loader, CRT |
| `window.create.begin` -> `shown` | 12.4 -> 18.1 | HWND, DPI, dark mode, backdrop, ShowWindow |
| `env.create.call` -> `env.created` | 18.1 -> 24.5 | `CreateCoreWebView2EnvironmentWithOptions` (**synchronous**: the completion callback runs inside the call) |
| `webview.controller.call` | 25.3 | everything before this is ours: 25 ms |
| `webview.controller.created` | 205.5 | **180 ms inside WebView2** (browser, GPU and utility processes start) |
| `webview.nav_starting` -> `asset.response` | 208.7 -> 210.2 | the boson:// request is answered in 0.1 ms |
| `webview.nav_completed` | 300.4 | **90 ms inside WebView2** (renderer process, parse, commit) |
| `page.first_frame` (2nd requestAnimationFrame) | 314.6 | first paint |

The part Boson controls is the first 25 ms (8 %). The other 92 % is the WebView2 runtime starting its processes. The
levers that exist are: do nothing on the UI thread that delays `controller.call`, and do not add work to the response of
the first request.

### What changed, with before / after

**1. `app::quit()` no longer waits for the browser process (shutdown).** Before, `quit` closed the controllers and waited up
to 3 s for `BrowserProcessExited`, only then `PostQuitMessage`. The browser takes 250-350 ms to exit on a quiet machine,
and the event often is not delivered at all under load (the 3 s timeout fired in most runs on a loaded machine), so the
UI thread's message loop was held for that long and the process lingered. Now `quit()` closes the controllers, releases
the environment and posts WM_QUIT immediately; the browser is told to close and flushes its profile and exits by itself,
as for any host that goes away. `Environment::shutdown(cb, 0ms)` is the same primitive (callback runs synchronously);
a positive timeout keeps the old waiting behaviour (`app::quit(envs, 3s)`, `WindowManager::quit(code, 3s)`);
`app::relaunch` still waits (its successor needs the user data folder, and `Environment` retries
`ERROR_INVALID_STATE` for a fast restart anyway).

| Shutdown, WM_CLOSE -> every process of the tree gone (`boson_bench`, 10 runs interleaved, quiet machine) | median | min-max |
|---|---:|---:|
| boson_bench_app, before | 325 ms | 310-341 |
| boson_bench_app, after | **171 ms** | 160-260 |
| boson_demo, before (8 runs) | 326 ms | 278-341 |
| boson_demo, after (8 runs) | **201 ms** | 171-245 |
| boson_browser, 10 tabs (scenario, 5 runs) before / after | 447 ms [382-484] | **304 ms** [243-336] |

The UI-thread view (QPC trace, WM_CLOSE -> last line of the process, 12 runs interleaved): before 308 ms median (min
270, one run 3017 = the timeout), after **16.7 ms** (11.7-58). At 100 % CPU load from other builds: before 2653 ms
(305-3035), after 20.5 ms (14.5-38). Wails measures ~205 ms to the same end point (docs/BENCHMARKS.md); the tree exit is
now below that.

**2. `app::exit_process(code)` + `app::add_exit_hook(fn)` (opt-in fast exit).** `return run_message_loop();` runs every
destructor of `wWinMain` (thread-pool joins, WebView and COM teardown, CRT atexit): **36.5 ms median (13-115)** between
`message_loop.exit` and the end of the process in the baseline. `exit_process` runs the registered hooks in reverse
order (each once, exceptions swallowed), flushes the BOSON_TRACE table and calls `ExitProcess`. Nothing that was not
flushed by a hook is saved, so use it only when the durable state is already on disk or covered by hooks:

- `Store` with `autosave` writes before `update()` returns; `WindowStateKeeper` saves synchronously on close (the
  demo registers `window_state.save_now()` as a hook anyway); `Logger`: register `logger.flush()`; a database: register its
  close/checkpoint; `CrashReporter` only installs a filter, nothing to flush.
- The bench app, the browser example and the demo use it. Measured tail after the message loop: 36.5 ms -> 0.

**3. First boson:// response pre-paid (startup).** The first `CreateWebResourceResponse` of a process costs ~2.5 ms
(stream marshalling is initialised) and every later one 0.03 ms. The UI thread is idle for ~180 ms while the controller is
created, so `ProtocolHandler::warm_up` does one throw-away call right after the controller request. The first asset
answer on the critical path went **3.0 ms -> 0.1 ms** (median of 7 runs; the warm-up itself, 2.9 ms, happens in the idle
window). Per-request cost of the handler (lookup + stream + headers + response) is ~0.1 ms, so zero-copy streams and
header caching were not pursued: there is nothing to win at that size.

**4. `exec::Scheduler` starts its pool on first use.** The constructor used to create `hardware_concurrency` threads
(24 here: 1.1-3.2 ms on the UI thread before the environment was even created). The pool is now created by the first
`get_scheduler()` (`std::call_once`). Start-up path: `webview.controller.call` 38 -> 34 ms median in the interleaved A/B
(min 29 -> 23.5; noise is +-5 ms). IPC unchanged (7 interleaved runs of the bench app's load test: 1000 concurrent
pings 88.5k vs 86.2k calls/s, 10 MB echo 205 vs 200 ms, sequential ping 0.1 ms both).

**5. Opt-in sleeping-tab mode `SleepPolicy::discard`** (and `WebView::discard()` / `is_discarded()`). Memory over wake
time: the sleeping WebView's controller is closed (its renderer process exits), the container, bounds and configuration
stay, and `set_visible(true)` creates a new controller and reloads the last URL (page state is lost, like Wails'
hidden windows). `lower_memory_target` and `suspend` are ignored in this mode. Default stays TrySuspend.
`boson_browser --sleep-discard` turns it on; `boson_bench --scenario browser` measures both (5 runs, quiet machine):

| 10 tabs, 9 asleep | TrySuspend (default) | discard | change |
|---|---:|---:|---:|
| whole-tree private WS | 223.2 MB [221.3-225.7] | **167.7 MB** [165.2-171.4] | -25 % |
| WebView2 tree working set | 552.4 MB | **433.1 MB** | -22 % |
| processes | 17 | **8** | -9 |
| 5 tabs, 4 asleep: whole-tree private WS | 192.0 MB | **150.6 MB** | -22 % |
| wake a sleeping tab -> 2 frames (median) | **20.0 ms** [16.8-24.2] | 70.3 ms [64.5-82.8] | +50 ms (page reload) |
| tab switch between awake tabs | 18.5 ms | 22.4 ms | within noise |

Wake with discard costs about a new tab (destroy + recreate here: 72 ms). Caveats: after tabs were slept and woken
once the 10-awake-tab tree is a little heavier with discard (912 vs 800 MB working set, +11 MB private) because the woken
renderers started cold; the 15 ms wake of TrySuspend is the right default for tabs the user returns to.

### Tried and dropped (measured, no win)

| Idea | Result |
|---|---|
| Create the Environment before the window (examples) | `CreateCoreWebView2EnvironmentWithOptions` is synchronous (5 ms); same UI thread, nothing overlaps. Reverted. |
| Pre-load `EmbeddedBrowserWebView.dll` on a pool thread (`Environment::prewarm`) | environment call 5.3 ms -> 5.5 ms (10 runs); the DLL is not the cost. Removed. |
| `/DELAYLOAD:dcomp.dll` | `trace.init` 15.0 -> 15.8 ms (12 runs), +0. Reverted. |
| Browser switches: `--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection`, `--disable-background-networking --disable-component-update --disable-sync --no-first-run --disable-default-apps`, `--in-process-gpu` | controller creation median 237.5 ms (control) vs 235.7 / 217.1 / 261.1 ms, 10 interleaved runs each, ranges overlap. No switch changed by default. |
| Zero-copy asset stream, cached header blocks | the handler is 0.1 ms per request after the first; nothing to gain. |
| Waiting for `BrowserProcessExited` before WM_QUIT | the cost being removed in change 1. |

### Binary size

Release + LTO, /MD: `boson_demo.exe` 1 370 624 -> 1 379 328 bytes (+8.7 KB), `boson_browser.exe` 1 290 752 -> 1 298 944
(+8.2 KB), `boson_bench_app.exe` 765 952 -> 773 120 (+7.2 KB, 0.9 %): the trace table writer, exit hooks and discard. A cost,
not a win; the trace code is the bulk of it.

### Using it

```cpp
// Ending an app: hide the window (instant for the user), close controllers, leave. No browser wait.
window.on(WindowEventKind::CloseRequested, [&](WindowEvent& e) {
    e.cancel = true;
    window.hide();
    app::quit({environment});                    // app::quit({environment}, 3s) to wait for the browser process
});
app::add_exit_hook([&] { logger.flush(); });     // optional, only with exit_process
app::exit_process(Window::run_message_loop());   // optional: skips the destructors, ~35 ms
```

Order work for start-up: create the Window, the Environment and the first WebView (`host.create`) before any slow
non-UI work (settings from disk, database open, plugin discovery). The browser then spends its ~180 ms starting while that
work runs, instead of after it. The UI thread must reach the message loop to receive the controller callback.

### Verification

Verified:
- Release + LTO build of every target is warning-free (/W4); Debug build of every target is warning-free.
- ctest (Debug, `-j2`, final code, quiet machine): **23 of 23 enabled suites pass** in 69 s (`boson_compat_typecheck` is
  disabled upstream), including the new `boson_lifecycle_tests` (39 checks: exit hooks, lazy pool, `quit` posts WM_QUIT in
  10 ms, `shutdown(0)`, a positive timeout still waits, discard + wake + `SleepPolicy::discard`). An earlier full run while
  the machine was at 100 % CPU had `boson_integration_tests` and `boson_security_tests` fail on "BrowserProcessExited not
  observed" (their own 3 s `env->shutdown` waits); both passed alone straight after (125/125, 315/315) and in the final run.
- Numbers above: interleaved A/B on the same machine, medians and ranges quoted, run counts in the tables.

Not verified / limits:
- Under heavy CPU load (other agents' builds at 100 %) the browser tree takes 2-10 s to exit in both versions; the tree-exit
  comparison above is from the quiet period, the UI-thread comparison also from the loaded one.
- `quit()` without waiting then a restart within ~300 ms (browser still exiting) relies on `Environment`'s
  `ERROR_INVALID_STATE` retry loop; it was not stress-tested end to end. `relaunch()` still waits.
- `exit_process` was exercised by the bench apps and the demo, not with a pending Store/Logger write.
- Windows 10, ARM64, composition hosting and multi-window (`WindowManager`) quit paths were not measured; the code path is
  the same `Environment::shutdown`.
- The environment-first ordering and browser-switch ideas were evaluated only on boson_bench_app.
