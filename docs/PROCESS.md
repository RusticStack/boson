# Child processes, sidecars and named pipes

The `boson::process` module covers Node's `child_process` and Electron's `utilityProcess`. It works from C++ first, and
from JS through the `Process` controller. It never uses a shell and has no Node dependency: Node is just one program
you can choose to bundle.

| Header | What |
|---|---|
| `boson/process/command_line.hpp` | argv quoting (CRT / `CommandLineToArgvW` rules, batch files via cmd.exe), environment blocks, `resolve_executable` |
| `boson/process/process.hpp` | `ProcessManager`, `Process`, `SpawnOptions`, `exec`, `LineSplitter` |
| `boson/process/sidecar.hpp` | `Sidecar`: ready checks, restart with backoff, graceful stop, logging, host pipe |
| `boson/process/pipe_ipc.hpp` | `PipeServer` / `PipeClient` / `PipeConnection` (overlapped, message mode), `allocate_loopback_port` |
| `boson/process/policy.hpp` | `ExecutablePolicy`: which programs pages may run (default deny) |
| `boson/ipc/controllers/process_controller.hpp` | JS `Process.*` and the `process:*` events |
| `frontend/src/process.ts`, `compat/child_process.ts`, `compat/electron.ts` | typed helpers, a Node `child_process` subset, and `utilityProcess.fork` |

## Design

- **Every wait is kernel-driven, and nothing polls.**
  - Pipe reads and writes use thread-pool I/O (`CreateThreadpoolIo`, IOCP).
  - Process exit is a `TP_WAIT` on the process handle, and timeouts are `TP_TIMER`s.
  - Sidecar port-file readiness uses `FindFirstChangeNotification`.
  - The module owns no threads. With a child idle, this process used 0 ms of CPU in 1 s.
- **Lazy.** The job object is created on the first spawn. Nothing exists before that.
- **Standard handles are overlapped named pipes on our side.**
  - Each pipe is named `\.pipeoson-stdio-<pid>-<n>-<qpc>` and created with `FIRST_PIPE_INSTANCE`, `REJECT_REMOTE_CLIENTS` and a current-user DACL.
  - The child gets ordinary synchronous handles.
  - Only the listed handles are inherited (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), so concurrent spawns from other threads never leak handles into each other's children.
- **Jobs.**
  - Every non-detached child is created suspended and assigned to the manager job (`KILL_ON_JOB_CLOSE`, `BREAKAWAY_OK`) and to its own nested job. It is resumed only after that.
  - If this process exits or crashes, the kernel kills every child and grandchild.
  - `kill(tree)` is `TerminateJobObject` on the nested job. For detached children it falls back to a toolhelp descendant walk that checks creation times, so a reused pid is never killed.
- **Never a shell.**
  - Arguments are quoted for the CRT / `CommandLineToArgvW`.
  - `.bat` / `.cmd` files run only with `allow_batch_files`. They go through `cmd.exe /e:ON /v:OFF /d /c ""script" args"`, escaped BatBadBut-style:
    - `%` becomes `%%cd:~,%`.
    - `"` is doubled.
    - Arguments containing CR, LF or NUL are rejected.
  - Program lookup never searches the current directory. It tries `.com` / `.exe` (and `.bat` / `.cmd` when allowed) on `PATH`, then the system and Windows directories.
- **Exceptions never cross a boundary.** Errors are `std::expected<T, ProcessError>`. A callback that throws is caught and logged, and controller handlers return JSON-RPC errors.

## C++

```cpp
#include <boson/process/process.hpp>
namespace bp = boson::process;

bp::ProcessManager pm; // one per app; children die with this process

// execFile: no shell, arguments are passed verbatim.
auto r = pm.exec_sync({.spawn = {.file = "git", .args = {"log", "-1", "--format=%H %s"}, .cwd = repo},
                       .max_buffer = 4 << 20});
if (r && r->status.exit_code == 0) use(r->stdout_data);

// Streaming, line mode; stdin stays open.
auto p = pm.spawn({.file = "C:\\tools\\lsp-server.exe",
                   .args = {"--stdio"},
                   .output_mode = bp::OutputMode::Line,
                   .high_water_bytes = 1 << 20,          // backpressure: stop reading until ack()
                   .on_lines = [](int fd, std::span<const std::string_view> lines) { /* fd 1 or 2 */ },
                   .on_exit = [](const bp::ExitStatus& s) { /* s.exit_code, s.killed, s.timed_out */ }});
(*p)->write(request_bytes);
(*p)->kill();                                            // the whole tree
auto status = stdexec::sync_wait((*p)->when_closed());   // sender: exit + all output delivered
```

`SpawnOptions` fields, in declaration order (designated initializers must follow it):

| Field | Description |
|---|---|
| `file`, `args` | The program and its arguments. |
| `cwd` | Working directory. |
| `env` / `env_remove` / `clear_env` | The environment block is sorted case-insensitively, as Windows requires. |
| `stdin_mode` / `stdout_mode` / `stderr_mode` | `Pipe`, `Inherit`, `Ignore` or `MergeWithStdout`. |
| `extra_handles` | fd 3, 4, ... Each is a pipe in the given direction. It reaches the child through the CRT `lpReserved2` table, so `_get_osfhandle(3)` / Node `fs.writeSync(3)` work, and through `BOSON_EXTRA_HANDLES=3=<hex>;4=<hex>`. Use `Process::write_to(fd)` and `on_data(fd, ...)`. |
| `node_ipc` | A Node `child_process.fork` channel: a duplex pipe plus `NODE_CHANNEL_FD`, JSON serialization and libuv framing. Use `send_message` / `on_message`. |
| `console`, `hide_window` | Default: hidden, with no new console window. |
| `detached` | Not in the job, so it survives this process. |
| `new_process_group` | Needed for `interrupt()` (Ctrl+Break). |
| `allow_batch_files` | Allow `.bat` / `.cmd` through cmd.exe. |
| `timeout`, `kill_exit_code` | On timeout the tree is killed and `timed_out` is set. |
| `output_mode` | `Chunk` or `Line`. Line mode handles CRLF and LF, lines split across reads, and `max_line_bytes`. |
| `read_buffer_bytes` | Read buffer size. |
| `high_water_bytes` | Flow control. Reading resumes when outstanding bytes drop to half of this. |
| Callbacks: `on_data` / `on_lines` / `on_message` / `on_exit` / `on_close` | They run on pool threads. Never block the UI thread with `wait()`; marshal through `UiMarshal` when the UI needs the result. |

`ProcessManager::exec()` and `Process::when_closed()` are stdexec senders, built on `exec::CallbackSender`. `exec_async` is the
callback form. If stdout or stderr exceeds `max_buffer`, the tree is killed and `max_buffer_exceeded` is set.

### Sidecars

```cpp
#include <boson/process/sidecar.hpp>
bp::Sidecar api(pm, {.name = "api",
                     .spawn = {.file = node, .args = {"server.js"}},
                     .restart = {.mode = bp::RestartMode::OnFailure, .max_restarts = 5},   // 250 ms, x2, max 30 s
                     .ready = {.kind = bp::ReadyKind::LineContains, .pattern = "listening", .timeout = 15s},
                     .stop = {.method = bp::StopMethod::CloseStdin, .grace = 3s},          // then kill
                     .allocate_port = true});                                              // BOSON_SIDECAR_PORT
api.start();
auto [ready] = stdexec::sync_wait(api.when_ready()).value();   // off the UI thread; or api.wait_ready(15s)
```

- **Ready checks:**
  - `Immediate`
  - `FirstLine`
  - `LineContains`
  - `PortFile`: the child writes `"<port>\n"` to `BOSON_SIDECAR_PORT_FILE`.
  - `PipeConnect`: the child connects to the pipe in `BOSON_SIDECAR_PIPE`. Only the sidecar's pid and its descendants are accepted, and other clients are disconnected.
  - A ready timeout kills the child, and that counts as a failure.
- **Stop methods:** `Kill`, `CtrlBreak`, `StdinLine` (sends a message), `CloseStdin`, and `PipeMessage`. After `grace`, the tree is killed.
- **Ctrl+Break details:**
  - It needs `new_process_group`, which the sidecar sets for this method.
  - If the child shares our console, `GenerateConsoleCtrlEvent` is called directly.
  - Otherwise a GUI app briefly attaches to the child's console, under a global lock.
  - Console apps whose child has its own console should use another stop method.
- **Restarts:**
  - Backoff is `min(initial * multiplier^n, max)`.
  - The counter resets after `reset_after` of uptime.
  - When restarts are exhausted, the state becomes `Failed`.
  - `restart()` kills the sidecar and starts it again right away.
- **Logging:** lifecycle events, plus stdout at Info and stderr at Warn (`log_output`), go to the shipping `Logger` under the category `sidecar:<name>`.

### Named pipes and ports

```cpp
auto server = bp::PipeServer::listen("my-app-rpc", [](std::shared_ptr<bp::PipeConnection> c) {
    c->start([c](std::span<const std::byte> msg) { c->send(msg); }, [] { /* closed */ });
}, {.message_mode = true, .current_user_only = true});
auto client = bp::PipeClient::connect("my-app-rpc", 2s);
(*client)->send(std::string_view("hello"));
std::uint16_t port = bp::allocate_loopback_port().value_or(0);
```

- **Message mode.** Messages larger than the pipe buffer are reassembled (`ERROR_MORE_DATA`), with `max_message_bytes` as the limit. Remote clients are rejected.
- **DACL.** By default only the current user and SYSTEM can open the pipe.
- **Peer pid.** `peer_pid()` is `GetNamedPipeClientProcessId`.
- **Port allocation is racy.** `allocate_loopback_port` binds 127.0.0.1:0 and closes the socket again, so another process can take the port in the meantime. Prefer `PortFile` readiness, where the child binds port 0 itself, when that matters.

## JS

Enable it in the app:

```cpp
auto policy = bp::ExecutablePolicy::create({
    .executables = {"C:\\Program Files\\Git\\cmd\\git.exe"},   // exact canonical paths
    .directories = {(app_dir / "sidecars").string()},           // everything below
    .commands = {"git"},                                         // bare names, resolved with PATH
    .max_processes = 16});
boson::ipc::ProcessController process_controller(pm, {.policy = *policy, .transport = &binary_transport});
// register in the Router, then:
connect_process_events(process_controller, bridge);
// capability (capabilities.json): "allow": ["Process.spawn", "Process.exec", "Process.write", "Process.close_stdin",
//                                           "Process.kill", "Process.ack", "Process.list"]  (+ "Process.send" for ipc)
```

- **Two gates.** The CapabilityPolicy decides which origins may call `Process.*`. The `ExecutablePolicy` decides which programs they may run.
- **ExecutablePolicy defaults:**
  - Deny everything (a null policy makes the controller inert).
  - No batch files, no detached processes.
  - Paths must be absolute. Bare names must be listed in `commands`.
  - `cwd` can be restricted with an `FsScope`, and an `ArgumentFilter` can veto arguments.
- **Policy from JSON.** `ExecutablePolicy::from_json` reads the same keys and rejects unknown ones.
- **Cleanup.** Processes started by pages are killed when the controller is destroyed.

```ts
import { spawn, exec, list } from './process';
const child = await spawn('git', ['log', '--oneline'], { output: 'line' });
child.onLines((lines) => render(lines));
const { code } = await child.exited;

const r = await exec('C:\\tools\\fmt.exe', ['--stdin'], { input: source, maxBuffer: 8 << 20, timeoutMs: 5000 });

// Node style
import { spawn as nodeSpawn, execFile, fork } from './compat/child_process';
execFile('git', ['status', '--porcelain'], (err, stdout) => { /* ... */ });
const worker = fork('C:\\app\\sidecars\\worker.js', [], { execPath: 'C:\\app\\sidecars\\node\\node.exe' });
worker.on('message', (m) => console.log(m)); worker.send({ hello: 1 });

// Electron style
import { utilityProcess } from './compat/electron';
const up = utilityProcess.fork('worker.js', [], { execPath: nodePath });
up.on('message', (m) => {}); up.postMessage({ job: 1 });
```

**Controller methods:**

| Method | Description |
|---|---|
| `Process.spawn` | Returns `{id, pid, executable}`. |
| `Process.exec` | Returns `{id, pid}` at once; the result arrives as a `process:exec` event. |
| `Process.write` | Writes to stdin. |
| `Process.close_stdin` | Closes stdin. |
| `Process.kill` | `SIGINT` / `SIGBREAK` send Ctrl+Break; any other signal terminates the tree. |
| `Process.ack` | Flow control acknowledgement. |
| `Process.send` | Sends a message to an ipc child. |
| `Process.list` | Lists the processes started by pages. |

**Events:**

| Event | Payload |
|---|---|
| `process:stdout` / `process:stderr` | `{id, stream, data | lines, encoding, bytes}` |
| `process:exit` | Sent after all output. `{code, killed, timed_out, signal, duration_ms}` |
| `process:exec` | The `Process.exec` result. |
| `process:message` | A message from an ipc child. |

Each event goes only to the WebView that started the process.

**Notes on the JS side:**
- **Encoding:** UTF-8 output is never split inside a character, and invalid bytes are replaced. `encoding: 'binary'` uses the `BinaryTransport` channel `process` when one is set, and base64 otherwise.
- **Early events:** handlers run on the pool and the router has no deferred replies, so events can arrive before `spawn()` resolves. `process.ts` buffers them by id and replays them.
- **Backpressure:** `process.ts` acknowledges output after the listeners have run. A page that stops handling output stops the native reader, and the child then blocks on a full pipe.
- **Ordering:** requests run concurrently. `ChildProcess.write`, `end` and `send` are chained in the order they are called. Raw `chrome.webview.postMessage` callers must wait for each reply.
- **`compat/child_process.ts` differences from Node:**
  - `exec()` and `shell: true` throw.
  - `pid` is set when `'spawn'` fires.
  - Chunks are strings, or `Uint8Array` with `encoding: 'buffer'`. They are not `Buffer`.
  - A replaced `env` starts from an empty environment.
  - `util.promisify(execFile)` works through `nodejs.util.promisify.custom`, and `execFileAsync` is exported.

## Bundling Node (or any runtime) as a sidecar

Boson does not need Node. If your app does, bundle a pinned `node.exe` with your scripts:

1. **Lay out the files.** Put `node.exe`, taken from the official `node-vXX-win-x64.zip` (about 80 MB, or smaller with a custom build), and your scripts in `third_party/node/`.
2. **Register the directory in CMake:**
   ```cmake
   include(cmake/BosonPackaging.cmake)
   boson_add_sidecar(my_app DIRECTORY third_party/node NAME node)   # copied to <exe dir>/sidecars/node on build
   boson_package(my_app NAME "My App" VERSION 1.0.0 VENDOR "Acme")  # installs sidecars/node (ZIP, MSI, MSIX)
   ```
   `boson_package(... SIDECAR_DIRECTORIES dir ...)` does the same for directories without a post-build copy.
3. **Find it at runtime.** `bp::bundled_sidecar_directory("node") / L"node.exe"` gives the path. Allow that directory in the `ExecutablePolicy` (`.directories`) if pages start it.
4. **Talk to it,** whichever way fits:
   - **child_process.fork IPC:** `SpawnOptions::node_ipc` / JS `fork` / `utilityProcess.fork`. In the child, use `process.send` / `process.on('message')`. Measured at 19 µs per pipelined round trip.
   - **A sidecar pipe:** `ReadyKind::PipeConnect`. The script does `net.connect(process.env.BOSON_SIDECAR_PIPE)` and the host uses `Sidecar::send` / `on_pipe_message`. Node pipes are byte streams, so frame the messages yourself. The test uses newline-delimited JSON.
   - **HTTP on `BOSON_SIDECAR_PORT`:** combine with `allocate_port` and `LineContains` readiness. `examples/process/main.cpp` shows this.
5. **Signing.** Sign `node.exe` along with your app (`boson_sign_command`). Pin the Node version, because the bundle does not update itself.

## Performance

These figures come from `boson_process_tests`, run on the Debug build (`build/wt`, MSVC /Od) on Windows 11 with other builds
running on the machine. Release builds should be faster for anything that costs CPU in our process.

| Measurement | Result |
|---|---|
| Child stdout to `on_data`, 256 MiB, chunk mode | 2.0 to 2.3 GiB/s (about 1200 callbacks with a 256 KiB buffer) |
| stdin to child to stdout round trip, 8 MiB random | 160 to 190 MiB/s (child is the Debug test exe doing 64 KiB reads) |
| Line mode, 64 MiB of 64-byte lines | 4.3 to 6.1 M lines/s (260 to 370 MiB/s) |
| Named pipe, 100k messages of 256 B, one way | 58k to 68k messages/s |
| Node fork IPC, 2000 pipelined JSON round trips | 15 to 19 µs each |
| Spawn, exit and close of a trivial child | 39 to 50 ms wall per process (most of it is the child's own startup); `spawn()` itself takes 8 to 9 ms |
| Sidecar ready (LineContains) / Node pipe sidecar ready | 84 ms / 47 to 72 ms |
| CPU used by this process during 1 s with an idle child holding three pipes | 0 ms |
| Backoff accuracy (100, 200, 400 ms policy) | +6 to 16 ms |

## Verification

**Tested.** `boson_process_tests` (registered with ctest, 398 checks) passes in full on the machine above. The test exe is also every
child process (`--child --mode`). It covers:

- **Quoting:**
  - `build_command_line` round-trips through `CommandLineToArgvW`: empty strings, spaces, tabs, quotes, trailing and embedded backslashes, unicode.
  - A real child (`__wargv`) sees exactly the arguments.
  - Batch-file arguments with `% ! ^ & | < > "` echo back exactly through a real `.bat`.
  - CR and LF are rejected.
- **Environment and lookup:** the environment block layout, and that executable lookup never uses the cwd.
- **Pipes:**
  - stdin/stdout round trip and throughput.
  - Line mode edge cases.
  - Merge and ignore.
  - Flow control pauses at exactly `high_water` and resumes on ack.
- **Extra handles:** fd 3 written and fd 4 read by the child through the CRT table.
- **Jobs:**
  - `kill(tree)` kills the child and its grandchild.
  - Kill-on-close: a host process is terminated, and its grandchild dies with the job.
  - Detached children stay out of the job and are still killed by the snapshot walk.
- **exec:**
  - `maxBuffer` truncates and kills.
  - Timeout kills the tree.
  - Input, exit codes, and the stdexec senders.
- **Policy:** default deny, exact paths, directories, PATH commands, batch files, cwd scope, the argument filter, and `from_json`.
- **Named pipes:**
  - Message mode from 0 B to 4 MiB in both directions.
  - Throughput.
  - Rejecting a client that is not the sidecar's process.
- **Port allocator.**
- **Sidecar:**
  - Backoff, then Failed.
  - LineContains, port and `when_ready`.
  - PortFile readiness.
  - PipeConnect with messages and PipeMessage stop.
  - Ready timeout, `restart()` and `Always`.
  - Ctrl+Break stop (exit code 42, with the child sharing the test console).
- **Node v26.13.0:**
  - fork IPC with libuv framing.
  - A bundled-Node style pipe sidecar.
  - These checks skip when `node` is not on PATH.
- **Controller through `Router`:**
  - The policy gate, `max_processes`, detached refused, unknown ids, invalid JSON, and TS generation.
- **Controller in a real WebView2 page, through `Bridge` and `CapabilityPolicy`:**
  - Line events with UTF-8.
  - Exit ordered after all output.
  - `process:exec` with NUL and quote bytes.
  - A denied executable.
  - `-32003` for a method the capability denies.
  - Kill from the page.

**Built and type-checked only, not run:** `process.ts`, `compat/child_process.ts` and `utilityProcess` pass `tsc --noEmit` and the Vite build,
but no page exercised them. The WebView test talks to the controller with raw `postMessage`.

**Not verified:**
- Ctrl+Break into a child with its own console from a GUI app (the `AttachConsole` path).
- Binary output through a live `BinaryTransport` (only the base64 fallback logic is compiled).
- `boson_add_sidecar` / `SIDECAR_DIRECTORIES` in an actual ZIP, MSI or MSIX package (the CMake code is configured but not packaged).
- Release-build numbers.
- Behaviour under a restrictive AppContainer (MSIX packages run as full-trust desktop apps).
