/**
 * Native terminals (boson::terminal::TerminalHost / TerminalController, ConPTY) for any renderer.
 *
 * Output arrives as Uint8Array chunks over the BinaryTransport shared buffers (no base64, no JSON per chunk).
 * A chunk's bytes are only valid until the listener returns, or until the Promise it returns settles: return
 * a Promise from a renderer that parses later (xterm.write(data, callback)) to keep it zero-copy. The native
 * side is credited (acked) once every listener is done with a chunk, so a slow renderer backpressures the
 * child process instead of flooding the page.
 *
 * The helper is renderer-agnostic: attachRenderer() takes anything with write(Uint8Array); attachXterm() wires
 * an xterm.js Terminal (use the WebGL addon for throughput); a ghostty-vt WASM or native GPU renderer plugs in
 * the same way.
 */
import { boson, type Unsubscribe } from './boson';
import { onBinary, type BinaryMessage } from './transfer';

export interface TerminalSpawnOptions {
  /** Executable (searched on PATH). Omit for the default shell (%COMSPEC%). */
  command?: string;
  args?: string[];
  /** A complete Windows command line; wins over command/args. */
  commandLine?: string;
  cwd?: string;
  /** Variables to add or override (an empty string removes one). */
  env?: Record<string, string>;
  /** Start from the host process environment (default true). */
  inheritEnv?: boolean;
  cols?: number;
  rows?: number;
  backend?: 'auto' | 'system' | 'bundled';
}

/** Terminal.info / Terminal.list entry. */
export interface TerminalInfo {
  id: number;
  pid: number;
  cols: number;
  rows: number;
  running: boolean;
  exit_code?: number;
  visible: boolean;
  command_line: string;
  backend: string;
  title: string;
  cwd: string;
}

interface SpawnResponse {
  terminal: TerminalInfo;
  channel: string;
  fast_input: boolean;
}

export interface TerminalProcesses {
  id: number;
  foreground_pid: number;
  foreground_name: string;
  cwd: string;
  cwd_source: '' | 'osc' | 'peb';
  processes: { pid: number; parent_pid: number; name: string }[];
}

export interface TerminalStats {
  bytes_in: number;
  bytes_sent: number;
  bytes_acked: number;
  bytes_dropped: number;
  chunks_sent: number;
  pending: number;
  in_flight: number;
  max_pending: number;
  max_in_flight: number;
  reader_pauses: number;
  replays: number;
  sync_holds: number;
  ring_capacity: number;
  visible: boolean;
  reader_paused: boolean;
}

/** Per-chunk metadata from the native OutputChannel. */
export interface TerminalChunkMeta {
  id: number;
  seq: number;
  /** Absolute stream offset after this chunk. */
  end: number;
  /** The renderer must be cleared before this chunk (replay, or bytes were dropped while hidden). */
  reset: boolean;
}

export type TerminalDataListener = (data: Uint8Array, meta: TerminalChunkMeta) => void | Promise<unknown>;

/** Anything that can draw VT output. write may return a Promise that settles once data was consumed. */
export interface TerminalRenderer {
  write(data: Uint8Array): void | Promise<unknown>;
  /** Clear screen and scrollback (called before reset chunks). */
  reset?(): void;
}

interface Listeners<T> {
  set: Set<T>;
}

const terminals = new Map<number, BosonTerminal>();
// terminal:exit can overtake the Terminal.spawn response for very short-lived commands.
const earlyExits = new Map<number, { code: number; end: number }>();
const installedChannels = new Set<string>();
let eventsInstalled = false;

// Fast-path messages: consumed by TerminalHost on the UI thread before the Bridge; "__boson_term" must be the first key.
function postInput(id: number, d: string): boolean {
  const wv = window.chrome?.webview;
  if (!wv) return false;
  wv.postMessage({ __boson_term: 'w', id, d });
  return true;
}

function postAck(id: number, n: number): boolean {
  const wv = window.chrome?.webview;
  if (!wv) return false;
  wv.postMessage({ __boson_term: 'a', id, n });
  return true;
}

function installChannel(channel: string): void {
  if (installedChannels.has(channel)) return;
  installedChannels.add(channel);
  onBinary<TerminalChunkMeta>(channel, (message: BinaryMessage<TerminalChunkMeta>) => {
    const term = terminals.get(message.meta.id);
    if (!term) {
      // Disposed (or never registered) terminal: keep the native credit accounting right.
      postAck(message.meta.id, message.data.byteLength);
      return undefined;
    }
    return term._deliver(message.data, message.meta);
  });
}

function installEvents(): void {
  if (eventsInstalled) return;
  eventsInstalled = true;
  boson.on<{ id: number; exit_code: number; end: number }>('terminal:exit', (e) => {
    const term = terminals.get(e.id);
    if (term) term._exit(e.exit_code, e.end);
    else if (earlyExits.size < 256) earlyExits.set(e.id, { code: e.exit_code, end: e.end });
  });
  boson.on<{ id: number; title: string }>('terminal:title', (e) => terminals.get(e.id)?._title(e.title));
  boson.on<{ id: number; cwd: string }>('terminal:cwd', (e) => terminals.get(e.id)?._cwd(e.cwd));
}

function add<T>(l: Listeners<T>, fn: T): Unsubscribe {
  l.set.add(fn);
  return () => {
    l.set.delete(fn);
  };
}

function settle(waits: Promise<unknown>[]): Promise<void> {
  return Promise.all(waits.map((w) => w.catch((err: unknown) => console.error('[boson] terminal listener rejected', err)))).then(() => undefined);
}

/** One native PTY. Create with spawnTerminal(). */
export class BosonTerminal {
  readonly id: number;
  readonly pid: number;
  readonly channel: string;
  readonly fastInput: boolean;
  cols: number;
  rows: number;
  title: string;
  cwd: string;
  exitCode: number | undefined;

  private readonly data: Listeners<TerminalDataListener> = { set: new Set() };
  private readonly exits: Listeners<(code: number) => void> = { set: new Set() };
  private readonly titles: Listeners<(title: string) => void> = { set: new Set() };
  private readonly cwds: Listeners<(cwd: string) => void> = { set: new Set() };
  private readonly resets: Listeners<() => void> = { set: new Set() };
  private wantVisible = false;
  private userHidden = false;
  private paused = false;
  private heldAcks = 0;
  private exitFired = false;
  private exitEnd = -1;
  private deliveredEnd = 0;
  private disposed = false;

  constructor(response: SpawnResponse) {
    this.id = response.terminal.id;
    this.pid = response.terminal.pid;
    this.channel = response.channel;
    this.fastInput = response.fast_input;
    this.cols = response.terminal.cols;
    this.rows = response.terminal.rows;
    this.title = response.terminal.title;
    this.cwd = response.terminal.cwd;
  }

  /**
   * Output chunks. The first listener makes the terminal visible (output buffered natively since spawn is
   * replayed to it). data is valid until the listener returns or its returned Promise settles.
   */
  onData(listener: TerminalDataListener): Unsubscribe {
    const off = add(this.data, listener);
    void this.syncVisibility();
    queueMicrotask(() => this.maybeFireExit());
    return () => {
      off();
      this.syncVisibility();
    };
  }

  /**
   * Called once with the exit code: after all output was delivered to the onData listeners, or immediately when
   * the terminal is hidden / has no listener (its backlog is then replayed when it is shown).
   */
  onExit(listener: (exitCode: number) => void): Unsubscribe {
    if (this.exitFired && this.exitCode !== undefined) {
      const code = this.exitCode;
      queueMicrotask(() => listener(code));
    } else {
      queueMicrotask(() => this.maybeFireExit()); // after the caller had the chance to attach onData too
    }
    return add(this.exits, listener);
  }

  /** OSC 0 / OSC 2 window title changes. */
  onTitle(listener: (title: string) => void): Unsubscribe {
    return add(this.titles, listener);
  }

  /** OSC 7 / OSC 9;9 working-directory reports from the shell. */
  onCwd(listener: (cwd: string) => void): Unsubscribe {
    return add(this.cwds, listener);
  }

  /** The renderer must clear itself (replay, or output dropped while hidden). Runs before that chunk's data. */
  onReset(listener: () => void): Unsubscribe {
    return add(this.resets, listener);
  }

  /** UTF-8 text / VT input. Uses the fire-and-forget fast path when the host attached it to this WebView. */
  write(data: string): void {
    if (this.disposed || data.length === 0) return;
    if (this.fastInput && postInput(this.id, data)) return;
    void boson.call('Terminal.write', { id: this.id, data }).catch(() => undefined);
  }

  /** Ctrl+C. */
  interrupt(): void {
    this.write('\x03');
  }

  resize(cols: number, rows: number): Promise<void> {
    const c = Math.max(1, Math.floor(cols));
    const r = Math.max(1, Math.floor(rows));
    if (c === this.cols && r === this.rows) return Promise.resolve();
    this.cols = c;
    this.rows = r;
    return boson.call('Terminal.resize', { id: this.id, cols: c, rows: r });
  }

  /** 'SIGINT' sends Ctrl+C; anything else terminates the process tree. */
  kill(signal?: string): Promise<void> {
    return boson.call('Terminal.kill', signal ? { id: this.id, signal } : { id: this.id });
  }

  /** Hidden panes buffer output natively (bounded ring) and get it replayed when shown again. */
  setVisible(visible: boolean): Promise<void> {
    this.userHidden = !visible;
    queueMicrotask(() => this.maybeFireExit());
    return this.syncVisibility();
  }

  /** Resends the retained output (reset chunk) - e.g. after re-creating the renderer. */
  replay(): Promise<void> {
    return boson.call('Terminal.replay', { id: this.id });
  }

  /** Stops crediting the native side: output backs up into the child process (node-pty pause()). */
  pause(): void {
    this.paused = true;
  }

  resume(): void {
    this.paused = false;
    const n = this.heldAcks;
    this.heldAcks = 0;
    if (n > 0) this.ack(n);
  }

  info(): Promise<TerminalInfo> {
    return boson.call('Terminal.info', { id: this.id });
  }

  processes(): Promise<TerminalProcesses> {
    return boson.call('Terminal.processes', { id: this.id });
  }

  stats(): Promise<TerminalStats> {
    return boson.call('Terminal.stats', { id: this.id });
  }

  /** Kills the process tree if it still runs and releases the native terminal. */
  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    terminals.delete(this.id);
    this.data.set.clear();
    await boson.call('Terminal.dispose', { id: this.id }).catch(() => undefined);
  }

  get disposedState(): boolean {
    return this.disposed;
  }

  // ---- internal (called by the channel / event dispatch) -------------------------------------------------

  /** @internal */
  _deliver(data: Uint8Array, meta: TerminalChunkMeta): void | Promise<void> {
    if (meta.reset) {
      for (const fn of [...this.resets.set]) {
        try {
          fn();
        } catch (err) {
          console.error('[boson] terminal reset listener threw', err);
        }
      }
    }
    const n = data.byteLength;
    const waits: Promise<unknown>[] = [];
    for (const fn of [...this.data.set]) {
      try {
        const r = fn(data, meta);
        if (r && typeof (r as Promise<unknown>).then === 'function') waits.push(r as Promise<unknown>);
      } catch (err) {
        console.error('[boson] terminal data listener threw', err);
      }
    }
    const done = (): void => {
      this.ack(n);
      if (meta.end > this.deliveredEnd) this.deliveredEnd = meta.end;
      this.maybeFireExit();
    };
    if (waits.length > 0) return settle(waits).then(done);
    done();
    return undefined;
  }

  /** @internal */
  _exit(code: number, end: number): void {
    this.exitCode = code;
    this.exitEnd = end;
    this.maybeFireExit();
  }

  /** @internal An exit that arrived before spawnTerminal() returned: decided once a listener attaches. */
  _pendingExit(code: number, end: number): void {
    this.exitCode = code;
    this.exitEnd = end;
    queueMicrotask(() => this.maybeFireExit());
  }

  /** @internal */
  _title(title: string): void {
    this.title = title;
    for (const fn of [...this.titles.set]) fn(title);
  }

  /** @internal */
  _cwd(cwd: string): void {
    this.cwd = cwd;
    for (const fn of [...this.cwds.set]) fn(cwd);
  }

  private ack(n: number): void {
    if (this.paused) {
      this.heldAcks += n;
      return;
    }
    if (this.fastInput && postAck(this.id, n)) return;
    void boson.call('Terminal.ack', { id: this.id, bytes: n }).catch(() => undefined);
  }

  private syncVisibility(): Promise<void> {
    const want = !this.userHidden && this.data.set.size > 0 && !this.disposed;
    if (want === this.wantVisible) return Promise.resolve();
    this.wantVisible = want;
    return boson.call<void, { id: number; visible: boolean }>('Terminal.set_visible', { id: this.id, visible: want }).catch(() => undefined);
  }

  // Shared-buffer chunks and web messages are not ordered against each other: the exit is reported once the
  // chunk reaching the final stream offset was rendered, or at once when nothing renders this terminal.
  private maybeFireExit(): void {
    if (this.exitFired || this.exitEnd < 0 || this.exits.set.size === 0) return;
    const rendering = this.data.set.size > 0 && !this.userHidden && !this.disposed;
    if (rendering && this.deliveredEnd < this.exitEnd) return;
    this.fireExit();
  }

  private fireExit(): void {
    if (this.exitFired || this.exitCode === undefined) return;
    this.exitFired = true;
    const code = this.exitCode;
    for (const fn of [...this.exits.set]) {
      try {
        fn(code);
      } catch (err) {
        console.error('[boson] terminal exit listener threw', err);
      }
    }
  }

}

/** Spawns a native PTY. It starts hidden, so nothing is lost before the first onData listener attaches. */
export async function spawnTerminal(options: TerminalSpawnOptions = {}): Promise<BosonTerminal> {
  installEvents();
  installChannel('terminal');
  const params: Record<string, unknown> = { visible: false };
  if (options.command !== undefined) params.command = options.command;
  if (options.args !== undefined) params.args = options.args;
  if (options.commandLine !== undefined) params.command_line = options.commandLine;
  if (options.cwd !== undefined) params.cwd = options.cwd;
  if (options.env !== undefined) params.env = options.env;
  if (options.inheritEnv !== undefined) params.inherit_env = options.inheritEnv;
  if (options.cols !== undefined) params.cols = Math.max(1, Math.floor(options.cols));
  if (options.rows !== undefined) params.rows = Math.max(1, Math.floor(options.rows));
  if (options.backend !== undefined) params.backend = options.backend;
  const response = await boson.call<SpawnResponse, Record<string, unknown>>('Terminal.spawn', params);
  installChannel(response.channel);
  const term = new BosonTerminal(response);
  terminals.set(term.id, term);
  const early = earlyExits.get(term.id);
  if (early) {
    earlyExits.delete(term.id);
    term._pendingExit(early.code, early.end);
  }
  return term;
}

/** Terminals this WebView spawned (including exited ones not yet disposed). */
export function listTerminals(): Promise<TerminalInfo[]> {
  return boson.call<{ terminals: TerminalInfo[] }>('Terminal.list').then((r) => r.terminals);
}

/** The BosonTerminal object for an id spawned in this page. */
export function getTerminal(id: number): BosonTerminal | undefined {
  return terminals.get(id);
}

export interface AttachOptions {
  /** Pane element: hidden (not intersecting the viewport) panes stop receiving output until shown again. */
  visibilityElement?: Element;
}

/** Wires a terminal to any renderer. Returns a function that detaches it. */
export function attachRenderer(term: BosonTerminal, renderer: TerminalRenderer, options: AttachOptions = {}): Unsubscribe {
  const offs: Unsubscribe[] = [];
  offs.push(term.onReset(() => renderer.reset?.()));
  offs.push(term.onData((data) => renderer.write(data)));
  const el = options.visibilityElement;
  if (el && typeof IntersectionObserver !== 'undefined') {
    const io = new IntersectionObserver((entries) => {
      const e = entries[entries.length - 1];
      if (e) void term.setVisible(e.isIntersecting && document.visibilityState !== 'hidden');
    });
    io.observe(el);
    offs.push(() => io.disconnect());
  }
  const onDoc = (): void => {
    if (document.visibilityState === 'hidden') void term.setVisible(false);
    else if (!el) void term.setVisible(true);
  };
  document.addEventListener('visibilitychange', onDoc);
  offs.push(() => document.removeEventListener('visibilitychange', onDoc));
  return () => {
    for (const off of offs.splice(0)) off();
  };
}

/** The subset of the xterm.js Terminal API attachXterm needs (xterm 5/6). */
export interface XtermLike {
  readonly cols: number;
  readonly rows: number;
  write(data: string | Uint8Array, callback?: () => void): void;
  reset(): void;
  onData(listener: (data: string) => void): { dispose(): void };
  onResize(listener: (size: { cols: number; rows: number }) => void): { dispose(): void };
}

/**
 * Wires an xterm.js Terminal: output (zero-copy, acked from xterm's write callback), keyboard input, resize and
 * visibility. Load @xterm/addon-webgl on the Terminal for GPU rendering.
 */
export function attachXterm(term: BosonTerminal, xterm: XtermLike, options: AttachOptions = {}): Unsubscribe {
  const offRenderer = attachRenderer(
    term,
    {
      write: (data) => new Promise<void>((resolve) => xterm.write(data, resolve)),
      reset: () => xterm.reset(),
    },
    options
  );
  const input = xterm.onData((d) => term.write(d));
  const resize = xterm.onResize(({ cols, rows }) => void term.resize(cols, rows));
  void term.resize(xterm.cols, xterm.rows);
  return () => {
    offRenderer();
    input.dispose();
    resize.dispose();
  };
}
