// Child processes from the page: typed helpers for the native Process controller (boson::ipc::ProcessController).
// Every program must be allowed by the app's ExecutablePolicy (default deny) and Process.* by its CapabilityPolicy.
// Arguments go straight to CreateProcessW: there is no shell, so no quoting or escaping is needed here.
import { boson, BosonIpcError, type Unsubscribe } from './boson';
import { onBinary } from './transfer';

export type StreamName = 'stdout' | 'stderr';

export interface SpawnOptions {
  cwd?: string;
  env?: Record<string, string>;
  /** Start from an empty environment (SystemRoot is kept). */
  clearEnv?: boolean;
  stdin?: 'pipe' | 'ignore';
  stdout?: 'pipe' | 'ignore';
  stderr?: 'pipe' | 'ignore' | 'merge';
  /** 'line': listeners get complete lines; 'chunk' (default): text or bytes as they arrive. */
  output?: 'chunk' | 'line';
  /** 'utf8' (default): strings. 'binary': Uint8Array (zero-copy shared buffers when a BinaryTransport is attached). */
  encoding?: 'utf8' | 'binary';
  /** Node child_process.fork() channel: send() / onMessage(). */
  ipc?: boolean;
  detached?: boolean;
  hideWindow?: boolean;
  /** Kill the process tree after this many milliseconds. */
  timeoutMs?: number;
  /** Backpressure (default true): the native side stops reading once highWaterBytes are not yet handled here. */
  flowControl?: boolean;
  highWaterBytes?: number;
  maxLineBytes?: number;
}

export interface ExitInfo {
  pid: number;
  code: number;
  killed: boolean;
  timedOut: boolean;
  signal: string | null;
  durationMs: number;
}

export interface ExecOptions {
  cwd?: string;
  env?: Record<string, string>;
  clearEnv?: boolean;
  /** Written to stdin, which is then closed. */
  input?: string | Uint8Array;
  timeoutMs?: number;
  /** Largest stdout (and stderr) kept; more kills the child (Node's maxBuffer). Default 1 MiB, 0 = unlimited. */
  maxBuffer?: number;
  /** 'utf8' (default) returns strings, 'buffer' returns Uint8Array. */
  encoding?: 'utf8' | 'buffer';
  hideWindow?: boolean;
}

export interface ExecResult<T extends string | Uint8Array = string> {
  pid: number;
  code: number;
  stdout: T;
  stderr: T;
  killed: boolean;
  timedOut: boolean;
  maxBufferExceeded: boolean;
  durationMs: number;
}

export interface ProcessListEntry {
  id: number;
  pid: number;
  executable: string;
  running: boolean;
  exec: boolean;
  exit_code?: number;
}

interface OutputEvent {
  id: number;
  stream: StreamName;
  data?: string;
  lines?: string[];
  encoding: 'utf8' | 'base64';
  bytes: number;
}

interface ExitEvent {
  id: number;
  pid: number;
  code: number;
  killed: boolean;
  timed_out: boolean;
  signal?: string;
  duration_ms: number;
}

interface ExecEvent {
  id: number;
  pid: number;
  code: number;
  stdout: string;
  stderr: string;
  encoding: 'utf8' | 'base64';
  killed: boolean;
  timed_out: boolean;
  max_buffer_exceeded: boolean;
  duration_ms: number;
}

interface MessageEvent_ {
  id: number;
  message: string;
}

type Dispatched =
  | { kind: 'output'; stream: StreamName; text?: string; lines?: string[]; bytes?: Uint8Array; size: number }
  | { kind: 'exit'; info: ExitInfo }
  | { kind: 'exec'; event: ExecEvent }
  | { kind: 'message'; message: unknown };

interface Sink {
  deliver(item: Dispatched): void;
}

const MAX_EARLY_ITEMS = 4096;
const EARLY_TTL_MS = 30_000;

/**
 * One subscription per event for all processes. Output can arrive before the spawn reply resolves (the child
 * starts writing immediately), so items for ids nobody claimed yet are kept briefly and replayed on claim.
 */
class ProcessRouter {
  private sinks = new Map<number, Sink>();
  private early = new Map<number, { at: number; items: Dispatched[] }>();
  private installed = false;

  public claim(id: number, sink: Sink): void {
    this.install();
    this.sinks.set(id, sink);
    const pending = this.early.get(id);
    if (pending) {
      this.early.delete(id);
      for (const item of pending.items) sink.deliver(item);
    }
  }

  public release(id: number): void {
    this.sinks.delete(id);
    this.early.delete(id);
  }

  public install(): void {
    if (this.installed) return;
    this.installed = true;
    const output = (e: OutputEvent): void => {
      if (e.encoding === 'base64') {
        this.route(e.id, { kind: 'output', stream: e.stream, bytes: decodeBase64(e.data ?? ''), size: e.bytes });
      } else {
        this.route(e.id, { kind: 'output', stream: e.stream, text: e.data, lines: e.lines, size: e.bytes });
      }
    };
    boson.on<OutputEvent>('process:stdout', output);
    boson.on<OutputEvent>('process:stderr', output);
    boson.on<ExitEvent>('process:exit', (e) =>
      this.route(e.id, {
        kind: 'exit',
        info: { pid: e.pid, code: e.code, killed: e.killed, timedOut: e.timed_out, signal: e.signal ?? null, durationMs: e.duration_ms },
      })
    );
    boson.on<ExecEvent>('process:exec', (e) => this.route(e.id, { kind: 'exec', event: e }));
    boson.on<MessageEvent_>('process:message', (e) => {
      let message: unknown = e.message;
      try {
        message = JSON.parse(e.message);
      } catch {
        // keep the raw text
      }
      this.route(e.id, { kind: 'message', message });
    });
    if (boson.isAvailable()) {
      onBinary<{ id: number; stream: StreamName; bytes: number }>('process', (m) => {
        // m.data views shared memory that is released when this listener returns: copy it.
        this.route(m.meta.id, { kind: 'output', stream: m.meta.stream, bytes: m.data.slice(), size: m.meta.bytes });
      });
    }
  }

  private route(id: number, item: Dispatched): void {
    const sink = this.sinks.get(id);
    if (sink) {
      sink.deliver(item);
      return;
    }
    const now = Date.now();
    for (const [key, entry] of this.early) {
      if (now - entry.at > EARLY_TTL_MS) this.early.delete(key);
    }
    let entry = this.early.get(id);
    if (!entry) {
      entry = { at: now, items: [] };
      this.early.set(id, entry);
    }
    if (entry.items.length < MAX_EARLY_ITEMS) entry.items.push(item);
  }
}

const router = new ProcessRouter();

function decodeBase64(text: string): Uint8Array {
  const raw = atob(text);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
  return out;
}

function encodeBase64(bytes: Uint8Array): string {
  let raw = '';
  const step = 0x8000;
  for (let i = 0; i < bytes.length; i += step) {
    raw += String.fromCharCode(...bytes.subarray(i, i + step));
  }
  return btoa(raw);
}

export type OutputListener = (data: string | Uint8Array, stream: StreamName) => void;
export type LinesListener = (lines: string[], stream: StreamName) => void;

/** A child started with spawn(). Listeners added right after spawn() resolves still see all output. */
export class ChildProcess implements Sink {
  public readonly exited: Promise<ExitInfo>;
  public exitInfo: ExitInfo | null = null;

  private outputListeners = new Set<OutputListener>();
  private lineListeners = new Set<LinesListener>();
  private messageListeners = new Set<(message: unknown) => void>();
  private resolveExit!: (info: ExitInfo) => void;
  private queue: Promise<unknown> = Promise.resolve();
  private pendingAck = { stdout: 0, stderr: 0 };
  private ackScheduled = false;
  private buffered: Dispatched[] = [];
  private flushScheduled = false;

  constructor(
    public readonly id: number,
    public readonly pid: number,
    public readonly executable: string,
    private readonly flowControl: boolean
  ) {
    this.exited = new Promise<ExitInfo>((resolve) => {
      this.resolveExit = resolve;
    });
    router.claim(id, this);
  }

  /** Output of both streams (chunk mode, or each line joined with '\n' in line mode). */
  public onOutput(listener: OutputListener): Unsubscribe {
    this.outputListeners.add(listener);
    this.scheduleFlush();
    return () => this.outputListeners.delete(listener);
  }

  public onStdout(listener: (data: string | Uint8Array) => void): Unsubscribe {
    return this.onOutput((data, stream) => {
      if (stream === 'stdout') listener(data);
    });
  }

  public onStderr(listener: (data: string | Uint8Array) => void): Unsubscribe {
    return this.onOutput((data, stream) => {
      if (stream === 'stderr') listener(data);
    });
  }

  /** Line mode (output: 'line'): batches of complete lines. */
  public onLines(listener: LinesListener): Unsubscribe {
    this.lineListeners.add(listener);
    this.scheduleFlush();
    return () => this.lineListeners.delete(listener);
  }

  /** Messages of an ipc child (process.send in Node). */
  public onMessage(listener: (message: unknown) => void): Unsubscribe {
    this.messageListeners.add(listener);
    this.scheduleFlush();
    return () => this.messageListeners.delete(listener);
  }

  public onExit(listener: (info: ExitInfo) => void): Unsubscribe {
    let active = true;
    void this.exited.then((info) => {
      if (active) listener(info);
    });
    return () => {
      active = false;
    };
  }

  /** Writes to stdin. Calls are applied in order. */
  public write(data: string | Uint8Array): Promise<{ ok: boolean; pending_bytes: number }> {
    const params =
      typeof data === 'string' ? { id: this.id, data, encoding: 'utf8' } : { id: this.id, data: encodeBase64(data), encoding: 'base64' };
    return this.enqueue(() => boson.call<{ ok: boolean; pending_bytes: number }, typeof params>('Process.write', params));
  }

  /** Optionally writes data, then closes stdin (the child reads end of file). */
  public end(data?: string | Uint8Array): Promise<boolean> {
    if (data !== undefined) void this.write(data);
    return this.enqueue(() => boson.call<{ ok: boolean }, { id: number }>('Process.close_stdin', { id: this.id })).then((r) => r.ok);
  }

  /** 'SIGINT' / 'SIGBREAK' send Ctrl+Break; anything else terminates (the whole tree unless tree is false). */
  public kill(signal: string = 'SIGTERM', tree = true): Promise<boolean> {
    return boson
      .call<{ ok: boolean }, { id: number; signal: string; tree: boolean }>('Process.kill', { id: this.id, signal, tree })
      .then((r) => r.ok);
  }

  /** One message to an ipc child (JSON). */
  public send(message: unknown): Promise<boolean> {
    const text = JSON.stringify(message ?? null);
    return this.enqueue(() => boson.call<{ ok: boolean }, { id: number; message: string }>('Process.send', { id: this.id, message: text })).then(
      (r) => r.ok
    );
  }

  /** @internal router callback */
  public deliver(item: Dispatched): void {
    if (item.kind === 'exit') {
      this.buffered.push(item);
      this.scheduleFlush();
      return;
    }
    const hasListener =
      item.kind === 'message' ? this.messageListeners.size > 0 : this.outputListeners.size > 0 || this.lineListeners.size > 0;
    if (!hasListener) {
      // Keep it until someone listens (the next microtask at the latest after spawn() resolved).
      this.buffered.push(item);
      return;
    }
    this.dispatch(item);
  }

  private dispatch(item: Dispatched): void {
    switch (item.kind) {
      case 'output': {
        const payload = item.bytes ?? (item.lines ? item.lines.join('\n') : (item.text ?? ''));
        if (item.lines && this.lineListeners.size > 0) {
          for (const l of [...this.lineListeners]) safe(() => l(item.lines ?? [], item.stream));
        }
        if (!item.lines || item.lines.length > 0) {
          for (const l of [...this.outputListeners]) safe(() => l(payload, item.stream));
        }
        this.ack(item.stream, item.size);
        break;
      }
      case 'message':
        for (const l of [...this.messageListeners]) safe(() => l(item.message));
        break;
      case 'exit':
        this.exitInfo = item.info;
        router.release(this.id);
        this.resolveExit(item.info);
        break;
      case 'exec':
        break;
    }
  }

  private scheduleFlush(): void {
    if (this.flushScheduled) return;
    this.flushScheduled = true;
    queueMicrotask(() => {
      this.flushScheduled = false;
      const items = this.buffered;
      this.buffered = [];
      for (const item of items) this.dispatch(item);
    });
  }

  private ack(stream: StreamName, bytes: number): void {
    if (!this.flowControl || bytes <= 0) return;
    this.pendingAck[stream] += bytes;
    if (this.ackScheduled) return;
    this.ackScheduled = true;
    queueMicrotask(() => {
      this.ackScheduled = false;
      for (const s of ['stdout', 'stderr'] as const) {
        const n = this.pendingAck[s];
        if (n > 0) {
          this.pendingAck[s] = 0;
          void boson.call('Process.ack', { id: this.id, stream: s, bytes: n }).catch(() => {});
        }
      }
    });
  }

  private enqueue<T>(fn: () => Promise<T>): Promise<T> {
    const next = this.queue.then(fn, fn);
    this.queue = next.catch(() => undefined);
    return next;
  }
}

function safe(fn: () => void): void {
  try {
    fn();
  } catch (err) {
    console.error('[boson] process listener threw', err);
  }
}

/** Starts command (an absolute path or a name the ExecutablePolicy lists) with args. Rejects with BosonIpcError. */
export async function spawn(command: string, args: string[] = [], options: SpawnOptions = {}): Promise<ChildProcess> {
  router.install();
  const flowControl = options.flowControl ?? true;
  const r = await boson.call<{ id: number; pid: number; executable: string }, Record<string, unknown>>('Process.spawn', {
    command,
    args,
    cwd: options.cwd ?? '',
    env: options.env ?? {},
    clear_env: options.clearEnv ?? false,
    stdin_mode: options.stdin ?? 'pipe',
    stdout_mode: options.stdout ?? 'pipe',
    stderr_mode: options.stderr ?? 'pipe',
    output: options.output ?? 'chunk',
    encoding: options.encoding ?? 'utf8',
    ipc: options.ipc ?? false,
    detached: options.detached ?? false,
    hide_window: options.hideWindow ?? true,
    timeout_ms: options.timeoutMs ?? 0,
    flow_control: flowControl,
    high_water_bytes: options.highWaterBytes ?? 1024 * 1024,
    max_line_bytes: options.maxLineBytes ?? 1024 * 1024,
  });
  return new ChildProcess(r.id, r.pid, r.executable, flowControl);
}

/** child_process.execFile: runs to completion and resolves with code, stdout and stderr (never rejects on a non-zero code). */
export function exec(command: string, args?: string[], options?: ExecOptions & { encoding?: 'utf8' }): Promise<ExecResult<string>>;
export function exec(command: string, args: string[], options: ExecOptions & { encoding: 'buffer' }): Promise<ExecResult<Uint8Array>>;
export async function exec(command: string, args: string[] = [], options: ExecOptions = {}): Promise<ExecResult<string | Uint8Array>> {
  router.install();
  const input = options.input ?? '';
  const r = await boson.call<{ id: number; pid: number }, Record<string, unknown>>('Process.exec', {
    command,
    args,
    cwd: options.cwd ?? '',
    env: options.env ?? {},
    clear_env: options.clearEnv ?? false,
    input: typeof input === 'string' ? input : encodeBase64(input),
    input_encoding: typeof input === 'string' ? 'utf8' : 'base64',
    timeout_ms: options.timeoutMs ?? 0,
    max_buffer: options.maxBuffer ?? 1024 * 1024,
    encoding: options.encoding === 'buffer' ? 'base64' : 'utf8',
    hide_window: options.hideWindow ?? true,
  });
  const event = await new Promise<ExecEvent>((resolve) => {
    router.claim(r.id, {
      deliver(item: Dispatched) {
        if (item.kind === 'exec') {
          router.release(r.id);
          resolve(item.event);
        }
      },
    });
  });
  const decode = (s: string): string | Uint8Array => (event.encoding === 'base64' ? decodeBase64(s) : s);
  return {
    pid: event.pid,
    code: event.code,
    stdout: decode(event.stdout),
    stderr: decode(event.stderr),
    killed: event.killed,
    timedOut: event.timed_out,
    maxBufferExceeded: event.max_buffer_exceeded,
    durationMs: event.duration_ms,
  };
}

/** Processes started by this app's pages that are still tracked. */
export async function list(): Promise<ProcessListEntry[]> {
  const r = await boson.call<{ processes: ProcessListEntry[] }>('Process.list');
  return r.processes;
}

export function isProcessError(err: unknown): err is BosonIpcError {
  return err instanceof BosonIpcError;
}

export const processes = { spawn, exec, list };
