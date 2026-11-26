/**
 * Node child_process compatibility subset on top of Boson's Process controller (frontend/src/process.ts).
 *
 *   import { spawn, execFile, fork } from './compat/child_process';   // or alias 'child_process' in the bundler
 *
 * Supported: spawn(command, args, { cwd, env, stdio: 'pipe' | 'ignore' | [..., 'ipc'], windowsHide, detached,
 * timeout, encoding }), execFile(file, args, options, callback) (+ util.promisify / execFileAsync),
 * fork(modulePath, args, { execPath, execArgv }) with send() / 'message'. ChildProcess emits 'spawn', 'error',
 * 'exit', 'close', 'message' and 'disconnect'; stdout / stderr emit 'data' and 'end'; stdin has write() / end().
 *
 * Differences from Node: there is never a shell (options.shell and exec() throw: pass the program and its
 * arguments to execFile / spawn instead); pid is set once 'spawn' fired (the call is asynchronous); data chunks
 * are strings unless options.encoding is 'buffer' (then Uint8Array, not Buffer); kill(signal) terminates the
 * whole process tree for every signal except 'SIGINT' / 'SIGBREAK' (Ctrl+Break); the program must be allowed by
 * the app's ExecutablePolicy; a replaced env (options.env) starts from an empty environment.
 */
import { BosonIpcError } from '../boson';
import { spawn as bosonSpawn, type ChildProcess as BosonChild, type ExitInfo } from '../process';

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type Listener = (...args: any[]) => void;

/** Minimal EventEmitter (on / once / off / emit). */
export class Emitter {
  private listeners = new Map<string, Listener[]>();

  public on(event: string, listener: Listener): this {
    const list = this.listeners.get(event) ?? [];
    list.push(listener);
    this.listeners.set(event, list);
    return this;
  }

  public addListener(event: string, listener: Listener): this {
    return this.on(event, listener);
  }

  public once(event: string, listener: Listener): this {
    const wrapped: Listener = (...args) => {
      this.off(event, wrapped);
      listener(...args);
    };
    return this.on(event, wrapped);
  }

  public off(event: string, listener: Listener): this {
    const list = this.listeners.get(event);
    if (list) {
      const i = list.lastIndexOf(listener);
      if (i >= 0) list.splice(i, 1);
      if (list.length === 0) this.listeners.delete(event);
    }
    return this;
  }

  public removeListener(event: string, listener: Listener): this {
    return this.off(event, listener);
  }

  public removeAllListeners(event?: string): this {
    if (event === undefined) this.listeners.clear();
    else this.listeners.delete(event);
    return this;
  }

  public listenerCount(event: string): number {
    return this.listeners.get(event)?.length ?? 0;
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  public emit(event: string, ...args: any[]): boolean {
    const list = this.listeners.get(event);
    if (!list || list.length === 0) {
      if (event === 'error') {
        console.error('[boson/child_process] unhandled error event', args[0]);
      }
      return false;
    }
    for (const listener of [...list]) {
      try {
        listener(...args);
      } catch (err) {
        console.error('[boson/child_process] "' + event + '" listener threw', err);
      }
    }
    return true;
  }
}

export class ReadableStream extends Emitter {
  private encoding: string | null = null;

  public setEncoding(encoding: string): this {
    this.encoding = encoding;
    return this;
  }

  /** @internal */
  public push(chunk: string | Uint8Array): void {
    if (this.encoding !== null && typeof chunk !== 'string') {
      this.emit('data', new TextDecoder().decode(chunk));
    } else {
      this.emit('data', chunk);
    }
  }

  public pipe<T extends { write(chunk: string | Uint8Array): unknown }>(target: T): T {
    this.on('data', (chunk: string | Uint8Array) => target.write(chunk));
    return target;
  }
}

export class WritableStream extends Emitter {
  constructor(private readonly target: () => Promise<BosonChild>) {
    super();
  }

  public write(chunk: string | Uint8Array, callback?: (err?: Error | null) => void): boolean {
    void this.target()
      .then((child) => child.write(chunk))
      .then(
        () => callback?.(null),
        (err: unknown) => {
          const error = err instanceof Error ? err : new Error(String(err));
          callback?.(error);
          this.emit('error', error);
        }
      );
    return true;
  }

  public end(chunk?: string | Uint8Array | (() => void), callback?: () => void): this {
    const done = typeof chunk === 'function' ? chunk : callback;
    const data = typeof chunk === 'function' ? undefined : chunk;
    void this.target()
      .then((child) => child.end(data))
      .then(
        () => {
          this.emit('finish');
          done?.();
        },
        (err: unknown) => this.emit('error', err instanceof Error ? err : new Error(String(err)))
      );
    return this;
  }
}

export interface SpawnOptions {
  cwd?: string;
  env?: Record<string, string | undefined>;
  stdio?: 'pipe' | 'ignore' | 'inherit' | Array<'pipe' | 'ignore' | 'inherit' | 'ipc' | null | undefined>;
  windowsHide?: boolean;
  detached?: boolean;
  timeout?: number;
  /** 'buffer' delivers Uint8Array chunks; anything else delivers strings. */
  encoding?: string;
  shell?: boolean | string;
  /** Accepted for compatibility; the tree is terminated for every signal but SIGINT / SIGBREAK. */
  killSignal?: string;
  windowsVerbatimArguments?: boolean;
}

function cleanEnv(env: Record<string, string | undefined> | undefined): Record<string, string> | undefined {
  if (!env) return undefined;
  const out: Record<string, string> = {};
  for (const [k, v] of Object.entries(env)) if (v !== undefined) out[k] = v;
  return out;
}

function stdioAt(stdio: SpawnOptions['stdio'], index: number): 'pipe' | 'ignore' {
  const value = Array.isArray(stdio) ? stdio[index] : stdio;
  return value === 'ignore' || value === 'inherit' ? 'ignore' : 'pipe';
}

function rejectShell(options: SpawnOptions | undefined, api: string): void {
  if (options?.shell) {
    throw new BosonIpcError(
      'child_process.' + api + ': options.shell is not supported (Boson never runs a shell; pass the program and its arguments)',
      -32602
    );
  }
}

export class ChildProcess extends Emitter {
  public pid: number | undefined = undefined;
  public exitCode: number | null = null;
  public signalCode: string | null = null;
  public killed = false;
  public connected = false;
  public readonly spawnfile: string;
  public readonly spawnargs: string[];
  public readonly stdin: WritableStream;
  public readonly stdout = new ReadableStream();
  public readonly stderr = new ReadableStream();
  private readonly ready: Promise<BosonChild>;

  constructor(file: string, args: string[], options: SpawnOptions, ipc: boolean) {
    super();
    this.spawnfile = file;
    this.spawnargs = [file, ...args];
    this.connected = ipc;
    this.ready = bosonSpawn(file, args, {
      cwd: options.cwd,
      env: cleanEnv(options.env),
      clearEnv: options.env !== undefined,
      stdin: stdioAt(options.stdio, 0),
      stdout: stdioAt(options.stdio, 1),
      stderr: stdioAt(options.stdio, 2),
      encoding: options.encoding === 'buffer' ? 'binary' : 'utf8',
      ipc,
      detached: options.detached ?? false,
      hideWindow: options.windowsHide ?? true,
      timeoutMs: options.timeout ?? 0,
    });
    this.stdin = new WritableStream(() => this.ready);
    this.ready.then(
      (child) => {
        this.pid = child.pid;
        child.onStdout((data) => this.stdout.push(data));
        child.onStderr((data) => this.stderr.push(data));
        child.onMessage((message) => this.emit('message', message));
        void child.exited.then((info: ExitInfo) => this.finish(info));
        this.emit('spawn');
      },
      (err: unknown) => {
        const error = err instanceof Error ? err : new Error(String(err));
        Object.assign(error, { code: 'ENOENT', path: file, spawnargs: args });
        this.connected = false;
        this.emit('error', error);
        this.emit('close', -2, null);
      }
    );
  }

  public kill(signal: string = 'SIGTERM'): boolean {
    if (this.exitCode !== null || this.signalCode !== null) return false;
    this.killed = true;
    void this.ready.then((child) => child.kill(signal)).catch((err: unknown) => this.emit('error', err));
    return true;
  }

  public send(message: unknown, callback?: (err: Error | null) => void): boolean {
    if (!this.connected) {
      callback?.(new Error('channel closed'));
      return false;
    }
    void this.ready
      .then((child) => child.send(message))
      .then(
        () => callback?.(null),
        (err: unknown) => callback?.(err instanceof Error ? err : new Error(String(err)))
      );
    return true;
  }

  public disconnect(): void {
    if (!this.connected) return;
    this.connected = false;
    this.emit('disconnect');
  }

  public ref(): void {}
  public unref(): void {}

  private finish(info: ExitInfo): void {
    this.exitCode = info.killed ? null : info.code;
    this.signalCode = info.killed ? (info.signal ?? 'SIGTERM') : null;
    this.stdout.emit('end');
    this.stderr.emit('end');
    this.disconnect();
    this.emit('exit', this.exitCode, this.signalCode);
    this.emit('close', this.exitCode, this.signalCode);
  }
}

export function spawn(command: string, args?: readonly string[] | SpawnOptions, options?: SpawnOptions): ChildProcess {
  const argv = Array.isArray(args) ? [...args] : [];
  const opts = (Array.isArray(args) ? options : (args as SpawnOptions | undefined)) ?? {};
  rejectShell(opts, 'spawn');
  const ipc = Array.isArray(opts.stdio) && opts.stdio.includes('ipc');
  return new ChildProcess(command, argv, opts, ipc);
}

export interface ExecFileOptions extends SpawnOptions {
  maxBuffer?: number;
  input?: string | Uint8Array;
}

export interface ExecFileError extends Error {
  code?: number | string;
  killed?: boolean;
  signal?: string | null;
  cmd?: string;
}

export type ExecFileCallback = (error: ExecFileError | null, stdout: string | Uint8Array, stderr: string | Uint8Array) => void;

function concat(chunks: Array<string | Uint8Array>, binary: boolean): string | Uint8Array {
  if (!binary) return chunks.join('');
  const total = chunks.reduce((n, c) => n + (typeof c === 'string' ? c.length : c.byteLength), 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const c of chunks) {
    const bytes = typeof c === 'string' ? new TextEncoder().encode(c) : c;
    out.set(bytes, offset);
    offset += bytes.byteLength;
  }
  return out;
}

/** Runs file to completion; callback(err, stdout, stderr) like Node (err for a non-zero code, kill, timeout, maxBuffer). */
export function execFile(
  file: string,
  args?: readonly string[] | ExecFileOptions | ExecFileCallback | null,
  options?: ExecFileOptions | ExecFileCallback | null,
  callback?: ExecFileCallback
): ChildProcess {
  const argv = Array.isArray(args) ? [...args] : [];
  let opts: ExecFileOptions = {};
  let cb: ExecFileCallback | undefined;
  for (const value of [args, options, callback]) {
    if (typeof value === 'function') cb = value as ExecFileCallback;
    else if (value && !Array.isArray(value) && typeof value === 'object') opts = value as ExecFileOptions;
  }
  rejectShell(opts, 'execFile');
  const binary = opts.encoding === 'buffer';
  const maxBuffer = opts.maxBuffer ?? 1024 * 1024;
  const child = new ChildProcess(file, argv, { ...opts, stdio: 'pipe' }, false);
  const out: Array<string | Uint8Array> = [];
  const err: Array<string | Uint8Array> = [];
  let sizes = [0, 0];
  let overflow = false;
  const collect = (target: Array<string | Uint8Array>, index: number) => (chunk: string | Uint8Array) => {
    if (overflow) return;
    sizes = index === 0 ? [sizes[0] + chunk.length, sizes[1]] : [sizes[0], sizes[1] + chunk.length];
    target.push(chunk);
    if (maxBuffer > 0 && sizes[index] > maxBuffer) {
      overflow = true;
      child.kill();
    }
  };
  child.stdout.on('data', collect(out, 0));
  child.stderr.on('data', collect(err, 1));
  const cmd = [file, ...argv].join(' ');
  child.on('spawn', () => {
    child.stdin.end(opts.input);
  });
  child.on('error', (e: Error) => cb?.(Object.assign(e, { cmd }), binary ? new Uint8Array() : '', binary ? new Uint8Array() : ''));
  child.on('exit', (code: number | null, signal: string | null) => {
    let error: ExecFileError | null = null;
    if (overflow) {
      error = Object.assign(new Error('stdout maxBuffer length exceeded'), { code: 'ERR_CHILD_PROCESS_STDIO_MAXBUFFER', killed: true, signal, cmd });
    } else if (code !== 0) {
      error = Object.assign(new Error('Command failed: ' + cmd), { code: code ?? undefined, killed: child.killed || signal !== null, signal, cmd });
    }
    cb?.(error, concat(out, binary), concat(err, binary));
  });
  return child;
}

/** Promise form of execFile: resolves {stdout, stderr}, rejects with the error (carrying stdout / stderr). */
export function execFileAsync(
  file: string,
  args: readonly string[] = [],
  options: ExecFileOptions = {}
): Promise<{ stdout: string | Uint8Array; stderr: string | Uint8Array }> {
  return new Promise((resolve, reject) => {
    execFile(file, args, options, (error, stdout, stderr) => {
      if (error) reject(Object.assign(error, { stdout, stderr }));
      else resolve({ stdout, stderr });
    });
  });
}

// util.promisify(execFile) behaves like Node's (resolves {stdout, stderr}).
Object.defineProperty(execFile, Symbol.for('nodejs.util.promisify.custom'), { value: execFileAsync });

/** Not supported: exec() runs its argument through a shell. Use execFile(program, args). */
export function exec(command: string): never {
  throw new BosonIpcError(
    "child_process.exec('" + command + "') runs a shell, which Boson never does; use execFile(program, args)",
    -32601
  );
}

export interface ForkOptions extends SpawnOptions {
  /** Node binary: an absolute path (e.g. a bundled sidecars/node/node.exe) or a name the ExecutablePolicy lists. Default 'node'. */
  execPath?: string;
  execArgv?: string[];
  silent?: boolean;
}

/** child_process.fork: runs modulePath with Node and an IPC channel (process.send / process.on('message') in the child). */
export function fork(modulePath: string, args: readonly string[] = [], options: ForkOptions = {}): ChildProcess {
  rejectShell(options, 'fork');
  const argv = [...(options.execArgv ?? []), modulePath, ...args];
  return new ChildProcess(options.execPath ?? 'node', argv, options, true);
}

export const child_process = { spawn, execFile, execFileAsync, exec, fork, ChildProcess };
export default child_process;
