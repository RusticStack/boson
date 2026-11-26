# Performance

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
