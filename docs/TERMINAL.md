# Terminals (terminal)

Native pseudo terminals (ConPTY) rendered in a WebView, for xterm.js-style panes in editors and agent UIs (a VS Code-like terminal panel, Orca ADE, T3 Code). There is no Node runtime and no node-pty.

| Layer | Where | What it does |
|---|---|---|
| `terminal::Pty` | `include/boson/terminal/pty.hpp` | One pseudo console with a process tree in a kill-on-close job. Overlapped I/O on Win32 thread-pool threads. |
| `terminal::OutputChannel` | `include/boson/terminal/output_channel.hpp` | Bounded ring between the PTY reader and a renderer. Credit/ack flow control, frame coalescing, hidden-pane gating, replay, DEC 2026, plus a VT side-channel scanner for title and cwd. |
| `terminal::TerminalHost` | `include/boson/terminal/terminal_host.hpp` | Many PTYs bound to WebViews. Output goes through `transfer::BinaryTransport` shared buffers, and input/acks take a fast path. |
| `ipc::TerminalController` (JS `Terminal.*`) | `include/boson/ipc/controllers/terminal_controller.hpp` | JSON-RPC surface. Emits the events `terminal:exit`, `terminal:title` and `terminal:cwd`. |
| `terminal.ts` | `frontend/src/terminal.ts` | Renderer-agnostic client: `spawnTerminal`, `attachRenderer`, `attachXterm`. |
| `compat/node_pty.ts` | `frontend/src/compat/node_pty.ts` | A node-pty-shaped API (`spawn`, `IPty`) for porting. |
| Demo | `examples/terminal_demo` | Split panes with xterm.js and the WebGL addon (`boson_terminal_demo`). |

## C++: Pty

```cpp
#include <boson/terminal/pty.hpp>
using boson::terminal::Pty;

boson::terminal::PtyOptions o;
o.command_line = L"pwsh.exe -NoLogo";           // empty: Pty::default_shell() (%COMSPEC%)
o.cwd = L"C:\src";
o.env = {{L"TERM_PROGRAM", L"myapp"}, {L"NODE_OPTIONS", L""}};  // override / remove ("" removes)
o.size = {.cols = 120, .rows = 30};
auto pty = Pty::spawn(o,
    [](std::span<const std::byte> out) { /* pool thread, one read at a time */ return boson::terminal::ReadControl::Continue; },
    [](std::uint32_t exit_code) { /* pool thread, after all output was delivered */ });
if (!pty) { /* pty.error(): "CreateProcess failed (Win32 error 2)" etc. */ }
(*pty)->write("dir");          // any thread, never blocks (queued overlapped writes)
(*pty)->resize({160, 40});
(*pty)->interrupt();             // Ctrl+C (, ConPTY raises CTRL_C_EVENT in the attached processes)
(*pty)->kill();                  // TerminateJobObject: the whole tree
auto fg = (*pty)->foreground_process();   // see the limits below
auto cwd = (*pty)->cwd();
```

- **Backends.** `PtyBackend::Auto` uses `conpty.dll` next to the executable when it is present, and otherwise the in-box `kernel32` ConPTY. `System` and `Bundled` force one or the other. The bundled DLL comes from the `Microsoft.Windows.Console.ConPTY` NuGet package and needs its `OpenConsole.exe` beside it. It is loaded with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | SYSTEM32` from the executable's directory only. Both the `CreatePseudoConsole` and `ConptyCreatePseudoConsole` export names are accepted. The bundled backend is about 20x faster (see Verification). Ship it with apps that stream a lot of output.
- **I/O.**
  - Our ends are overlapped named pipes (`FILE_FLAG_FIRST_PIPE_INSTANCE`, `PIPE_REJECT_REMOTE_CLIENTS`) bound to `CreateThreadpoolIo`. One read is outstanding at a time, with a 64 KiB buffer.
  - Writes are queued and chained, so the caller never blocks. Input is capped by `max_pending_input_bytes`.
  - Process exit is a thread-pool wait on the process handle. There are no threads of our own and no polling: an idle terminal costs nothing.
- **Exit ordering.** When the process exits, `ClosePseudoConsole` runs on a pool thread and the reader drains until `ERROR_BROKEN_PIPE`. `on_exit` fires only after both. All output always precedes the exit callback.
- **Kill-on-close job.**
  - The child is created suspended, assigned to a job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then resumed. No grandchild can escape before assignment.
  - `~Pty()`, `kill()` and a crash of the host process each end the whole tree.
- **Backpressure.** The output handler returns `ReadControl::Pause` to stop reading. ConPTY's pipe then fills and the child blocks in `WriteFile`. `resume_reading()` continues.
- **Helpers.** `quote_argument`, `build_command_line` (CommandLineToArgvW rules), `build_environment_block` (sorted, case-insensitive overrides) and `bundled_conpty_available` / `system_conpty_available`.

### Limits of the process queries

- `processes()` lists the job's processes (`JobObjectBasicProcessIdList`), including ones that re-parented.
- `foreground_process()` is a heuristic: it returns the newest process in the job. Windows has no foreground process group for a pseudo console, so `node server.js &`-style background children started later can win. The shell itself is returned when it is alone.
- `cwd()`:
  - Prefers what the shell reports with OSC 7 (`file://host/C:/dir`) or OSC 9;9 (Windows Terminal style). Configure the shell prompt for it.
  - Otherwise it reads `RTL_USER_PROCESS_PARAMETERS.CurrentDirectory` from the PEB of a 64-bit process. This is an undocumented layout that has been stable since Windows 7. WOW64 processes are rejected, and the call fails without `PROCESS_VM_READ` (for example, elevated children).
  - The PEB value is the process's own current directory. For cmd.exe it is exact. PowerShell's `Set-Location` does not change it, so use OSC 9;9 there.

## Flow control (OutputChannel)

```
ConPTY pipe -> pool read -> ring (1 MiB) -> [credit window 1 MiB] -> shared buffer chunk -> page -> ack(n)
```

- **Ring.** The ring holds undelivered output plus the replay history, by absolute stream offsets.
  - Visible: when undelivered bytes reach 75% of the ring, the reader pauses and the child blocks. It resumes at 50% or less.
  - Memory per terminal is therefore bounded by the ring plus the window, whatever the child writes.
- **Sends.**
  - Chunks are at most 256 KiB and at least 4 KiB of credit (no dribbling).
  - Nagle-style coalescing: when nothing is in flight, output is sent at once (typing echo). While the renderer is busy, output gathers until its ack, at most one frame (16 ms), or until 64 KiB piled up.
  - Sends run on a pool thread (posted through `TaskRunner`) or from a one-shot thread-pool timer, never on the UI thread and never while idle.
- **Copying.** There is one memcpy, from the ring into a pooled `ICoreWebView2SharedBuffer` (`BinaryTransport::send_binary_with`). No base64 and no JSON wrapping of the bytes. Chunk metadata is `{"id","seq","end","reset"}`, where `end` is the absolute offset after the chunk.
- **Hidden panes.**
  - Nothing is sent, and the reader never pauses. When the ring overflows, the oldest bytes are dropped (`bytes_dropped`).
  - On show, the backlog is sent. If anything was dropped, the first chunk carries `reset: true` and the renderer clears itself before writing. That chunk is the last ring's worth of output, so a TUI repaints correctly at the next frame.
  - `replay()` (a page reload or a new renderer) resends the whole ring with `reset`.
- **DEC 2026.** Output between `CSI ? 2026 h` and `CSI ? 2026 l` is held until the update ends, or for at most 150 ms, so a full-screen repaint arrives as one chunk.
- **Scanner.** `VtScanner` finds OSC 0/2 (title) and OSC 7 / 9;9 (cwd) with `memchr` over plain text. Sequences may be split across reads.

## Wiring a host

```cpp
boson::transfer::BinaryTransport transport(runner, marshal);
boson::terminal::TerminalHost terminals(runner, transport /*, TerminalHostOptions{...}*/);
boson::ipc::TerminalController terminal_controller(terminals);
boson::ipc::Router<boson::ipc::TerminalController, boson::ipc::TransferController> router(terminal_controller, transfer_controller);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview);
terminals.set_bridge(&bridge);           // terminal:exit / title / cwd go to the WebView that spawned the terminal
// once the WebView is ready:
transport.attach(webview);
terminals.attach(webview);               // fast input/ack path; detach(webview) kills its terminals
```

`examples/terminal_demo/main.cpp` is a complete app. `TerminalHostOptions::spawn_filter` can restrict what a page may run (for example, an allow-list of shells). `TerminalAttachOptions::capabilities` applies a `CapabilityPolicy` to the fast path as `Terminal.write` / `Terminal.ack`.

## JS API

| Method | Params | Result |
|---|---|---|
| `Terminal.spawn` | `command?, args?, command_line?, cwd?, env?, inherit_env?, cols?, rows?, visible?, backend?` | `{terminal, channel, fast_input}` |
| `Terminal.write` | `id, data` | `void` (prefer the fast path) |
| `Terminal.resize` | `id, cols, rows` | `void` |
| `Terminal.kill` | `id, signal?` (`"SIGINT"` sends Ctrl+C, anything else kills the tree) | `void` |
| `Terminal.set_visible` | `id, visible` | `void` |
| `Terminal.ack` | `id, bytes` | `void` (prefer the fast path) |
| `Terminal.replay`, `Terminal.dispose`, `Terminal.info`, `Terminal.processes`, `Terminal.stats` | `id` | |
| `Terminal.list`, `Terminal.capabilities` | | |

Events: `terminal:exit {id, exit_code, end}`, `terminal:title {id, title}`, `terminal:cwd {id, cwd}`. A page can only use terminals that it spawned itself. Other ids are reported as `unknown terminal`.

**Fast path.** `chrome.webview.postMessage({__boson_term:'w', id, d:'text'})` and `{__boson_term:'a', id, n:bytes}` are consumed on the UI thread before the Bridge. Only a prefix compare plus a small glaze parse runs there, followed by an overlapped write or a counter update. A keystroke costs no JSON-RPC round trip and no worker hop.

```ts
import { spawnTerminal, attachXterm } from './terminal';
import { Terminal } from '@xterm/xterm';
import { WebglAddon } from '@xterm/addon-webgl';

const xterm = new Terminal();
xterm.open(el);
xterm.loadAddon(new WebglAddon());
const term = await spawnTerminal({ commandLine: 'pwsh -NoLogo', cols: xterm.cols, rows: xterm.rows });
const detach = attachXterm(term, xterm, { visibilityElement: el });  // output, input, resize, visibility
term.onExit((code) => console.log('exit', code));
```

- **Zero copy.** Each chunk is the shared buffer itself. `attachXterm` hands it to `xterm.write(data, callback)` and acks from the callback, so the buffer is not copied. A slow renderer backpressures the child.
- **Start hidden.** `spawnTerminal` always spawns hidden. The first `onData` listener makes the terminal visible, and everything since spawn is delivered. Panes that scroll out of view (`IntersectionObserver`) or a hidden document stop receiving output until shown.
- **Exit after output.** `onExit` fires after the chunk reaching the exit's `end` offset was rendered, because shared-buffer messages and JSON events are not ordered against each other. When nothing renders the terminal, it fires immediately.
- **Other renderers.** `attachRenderer(term, {write, reset})` takes any renderer: a ghostty-vt WASM build, a canvas renderer or a headless parser.
- **node-pty port.** `compat/node_pty.ts` gives `pty.spawn(file, args, {cols, rows, cwd, env, encoding})` with `onData`, `onExit`, `write`, `resize`, `kill`, `pause`, `resume`. Its differences from node-pty are listed in its header: spawn is asynchronous (`ready`), env is merged, and `pause` is real backpressure.

## Verification

All numbers come from a Debug build (MSVC 14.52, `/W4`) on Windows 11 Pro 26300 with WebView2 158 on a developer machine that other builds were using at the same time. Treat them as orders of magnitude.

`boson_terminal_tests` (236 checks, passing):

| Measurement | in-box ConPTY (kernel32) | bundled conpty.dll 1.25.261109001 |
|---|---|---|
| `echo` command round trip (cmd.exe) | 15.1 - 15.7 ms | 0.4 - 0.7 ms |
| Typed text -> ConPTY echo (Pty level) | median 0.15 ms | median 0.10 ms |
| Typed in page -> echoed bytes back in page (fast path, full stack) | median 0.5 - 1.0 ms, max 15.6 ms | median 0.6 ms, max 1.0 ms |
| Raw ConPTY throughput, 64 MiB, no renderer | 18.7 - 19.0 MiB/s | 296 - 308 MiB/s |
| End to end, 128 MiB: child -> ConPTY -> ring -> shared buffer -> page, acked | 13.6 - 20.4 MiB/s | 302 - 307 MiB/s |
| `kill()` / `~Pty()`: 3-process tree gone | 3 - 6 ms / 1.5 - 3.2 ms | 3.2 ms / 1.6 ms |

- **End-to-end throughput.** It is bounded by ConPTY itself. The in-box ConPTY renders at about 60 Hz, which is also why its command echo takes one frame.
  - 128 MiB used about 1000 - 2200 chunks (avg 60 - 127 KiB), 0 base64 sends, 5 - 7 shared buffers created and the rest reused.
  - The page's private bytes rose by at most +2 MiB.
- **Pipeline ceiling.** A synthetic 256 MiB source pushed into `OutputChannel` (ring, shared buffers, page acks, without ConPTY) ran at 1.7 - 3.8 GB/s. Backlog stayed at most 768 KiB, in flight at most 1 MiB, with +1 - 2 MiB private bytes.
- **Slow renderer.** The page acks one chunk per 25 ms. 24 MiB passed at 4.4 - 5.0 MiB/s with 54 - 67 reader pauses. Backlog stayed at most 831 KiB, in flight at most 1 MiB, with +1 - 2 MiB private bytes. The child was blocked rather than buffered.
- **Hidden panes.** Nothing is delivered while hidden, and the replay on show is byte-exact. An overflow while hidden drops the oldest bytes and sends `reset`.
- **Other covered behavior.**
  - Exit codes, with output delivered before exit, and UTF-8 output and input (Latin, ✓, CJK and an astral emoji).
  - Resize as seen by the child, env and cwd, PEB cwd, foreground process while `ping` runs, and Ctrl+C stopping ping while the shell survives.
  - Controller over JSON-RPC: errors, ownership, capabilities.
  - The `terminal.ts` / `node_pty.ts` self-test in the WebView (17 assertions).
- **Bundled backend run.** The bundled column was measured by copying `conpty.dll` and `OpenConsole.exe` from the NuGet package (Authenticode: Microsoft Corporation, valid) next to a copy of the test executable. The package is not part of the repository or the build.

`boson_terminal_demo --selftest[=capture.png]` (registered as the `boson_terminal_demo_selftest` test) opens cmd.exe and PowerShell panes with xterm.js 6 and the WebGL addon. It types into both, hides a pane while 200 lines are printed (0 bytes delivered), shows it again (replay ends with `hidden-line-200`) and confirms that both panes use the WebGL renderer.

Not verified:
- Real keyboard input from a person. All input was synthesized through the same `postMessage` path xterm uses.
- IME composition and mouse reporting in TUIs (these are handled by xterm.js and pass through as input bytes).
- Long-running memory over hours.
- Elevated children (`cwd()` is expected to fail there).
- ARM64.
- Windows 10, where the in-box ConPTY is older: DEC 2026 passthrough and some sequences depend on its version.
- Release-build numbers.
