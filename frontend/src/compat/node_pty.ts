/**
 * node-pty compatible surface over Boson's native terminals (terminal.ts), for porting Electron apps that
 * use node-pty (Orca ADE, T3 Code, VS Code-style terminals) without a Node runtime.
 *
 *   import * as pty from './compat/node_pty';
 *   const p = pty.spawn('powershell.exe', [], { cols: 120, rows: 30, cwd: 'C:\\src' });
 *   p.onData((text) => xterm.write(text));
 *   p.onExit(({ exitCode }) => console.log('exited', exitCode));
 *   p.write('dir\r');
 *
 * Differences from node-pty, all because the native spawn is asynchronous here:
 *  - spawn() returns at once; pid is 0 until `ready` resolves. write()/resize() before that are queued.
 *  - `process` is the spawned file name; use getForegroundProcess() for the live foreground process.
 *  - env is merged into the host environment (node-pty replaces it); pass inheritEnv: false to replace.
 *  - onData receives strings decoded as a UTF-8 stream (a chunk boundary never splits a character), or
 *    Uint8Array copies with encoding: null.
 *  - pause()/resume() stop/restart crediting the native side, so the child blocks (real backpressure).
 * For zero-copy rendering use terminal.ts (attachXterm) directly.
 */
import { spawnTerminal, type BosonTerminal } from '../terminal';

export interface IDisposable {
  dispose(): void;
}

export type IEvent<T> = (listener: (e: T) => unknown) => IDisposable;

export interface IPtyForkOptions {
  name?: string;
  cols?: number;
  rows?: number;
  cwd?: string;
  env?: Record<string, string | undefined>;
  /** 'utf8' (default) or null for Uint8Array chunks. */
  encoding?: string | null;
  /** Accepted for compatibility; Boson always uses ConPTY. */
  useConpty?: boolean;
  /** Accepted for compatibility (flow control is always on natively). */
  handleFlowControl?: boolean;
  /** Boson extension: false replaces the environment instead of merging. */
  inheritEnv?: boolean;
}

export type IWindowsPtyForkOptions = IPtyForkOptions;

export interface IExitEvent {
  exitCode: number;
  signal?: number;
}

export interface IPty {
  readonly pid: number;
  readonly cols: number;
  readonly rows: number;
  readonly process: string;
  handleFlowControl: boolean;
  readonly onData: IEvent<string>;
  readonly onExit: IEvent<IExitEvent>;
  resize(columns: number, rows: number): void;
  clear(): void;
  write(data: string | Uint8Array): void;
  kill(signal?: string): void;
  pause(): void;
  resume(): void;
  /** Boson extension: resolves once the native PTY exists (rejects when the spawn failed). */
  readonly ready: Promise<void>;
  /** Boson extension: the live foreground process name (heuristic, see docs/TERMINAL.md). */
  getForegroundProcess(): Promise<string>;
  /** Boson extension: the underlying terminal (after ready). */
  readonly terminal: BosonTerminal | undefined;
}

class Emitter<T> {
  private readonly listeners = new Set<(e: T) => unknown>();
  readonly event: IEvent<T> = (listener) => {
    this.listeners.add(listener);
    return { dispose: () => this.listeners.delete(listener) };
  };
  get size(): number {
    return this.listeners.size;
  }
  fire(e: T): void {
    for (const l of [...this.listeners]) {
      try {
        l(e);
      } catch (err) {
        console.error('[boson] node-pty listener threw', err);
      }
    }
  }
}

function quoteArg(arg: string): string {
  if (arg.length > 0 && !/[\s"]/.test(arg)) return arg;
  let out = '"';
  for (let i = 0; ; ++i) {
    let backslashes = 0;
    while (i < arg.length && arg[i] === '\\') {
      ++i;
      ++backslashes;
    }
    if (i === arg.length) {
      out += '\\'.repeat(backslashes * 2);
      break;
    }
    if (arg[i] === '"') out += '\\'.repeat(backslashes * 2 + 1) + '"';
    else out += '\\'.repeat(backslashes) + arg[i];
  }
  return out + '"';
}

class BosonPty implements IPty {
  pid = 0;
  cols: number;
  rows: number;
  readonly process: string;
  handleFlowControl = false;
  readonly ready: Promise<void>;
  terminal: BosonTerminal | undefined;

  private readonly dataEmitter = new Emitter<string>();
  private readonly exitEmitter = new Emitter<IExitEvent>();
  private readonly queue: (() => void)[] = [];
  private readonly decoder = new TextDecoder('utf-8');
  private readonly binary: boolean;
  private killed = false;
  private pausedBeforeReady = false;

  readonly onData: IEvent<string> = (listener) => this.dataEmitter.event(listener);
  readonly onExit: IEvent<IExitEvent> = (listener) => this.exitEmitter.event(listener);

  constructor(file: string, args: string[] | string, options: IPtyForkOptions) {
    this.cols = options.cols ?? 80;
    this.rows = options.rows ?? 24;
    this.process = file;
    this.binary = options.encoding === null;
    const commandLine = typeof args === 'string' ? quoteArg(file) + (args ? ' ' + args : '') : [file, ...args].map(quoteArg).join(' ');
    const env: Record<string, string> = {};
    for (const [k, v] of Object.entries(options.env ?? {})) {
      if (v !== undefined) env[k] = v;
    }
    this.ready = spawnTerminal({
      commandLine,
      cwd: options.cwd,
      env: options.env ? env : undefined,
      inheritEnv: options.inheritEnv ?? true,
      cols: this.cols,
      rows: this.rows,
    }).then((term) => {
      this.terminal = term;
      this.pid = term.pid;
      if (this.pausedBeforeReady) term.pause();
      term.onData((data) => {
        if (this.binary) {
          (this.dataEmitter as unknown as Emitter<Uint8Array>).fire(data.slice());
        } else {
          const text = this.decoder.decode(data, { stream: true });
          if (text.length > 0) this.dataEmitter.fire(text);
        }
      });
      term.onExit((exitCode) => {
        const tail = this.binary ? '' : this.decoder.decode();
        if (tail.length > 0) this.dataEmitter.fire(tail);
        this.exitEmitter.fire({ exitCode, signal: this.killed ? 1 : 0 });
        void term.dispose();
      });
      for (const op of this.queue.splice(0)) op();
    });
    this.ready.catch((err: unknown) => {
      console.error('[boson] node-pty spawn failed', err);
      this.exitEmitter.fire({ exitCode: -1 });
    });
  }

  private whenReady(op: (t: BosonTerminal) => void): void {
    if (this.terminal) op(this.terminal);
    else this.queue.push(() => this.terminal && op(this.terminal));
  }

  resize(columns: number, rows: number): void {
    this.cols = Math.max(1, Math.floor(columns));
    this.rows = Math.max(1, Math.floor(rows));
    this.whenReady((t) => void t.resize(this.cols, this.rows).catch(() => undefined));
  }

  clear(): void {
    // node-pty: no-op on ConPTY as well.
  }

  write(data: string | Uint8Array): void {
    const text = typeof data === 'string' ? data : new TextDecoder().decode(data);
    this.whenReady((t) => t.write(text));
  }

  kill(signal?: string): void {
    this.killed = signal !== 'SIGINT';
    this.whenReady((t) => void t.kill(signal).catch(() => undefined));
  }

  pause(): void {
    if (this.terminal) this.terminal.pause();
    else this.pausedBeforeReady = true;
  }

  resume(): void {
    this.pausedBeforeReady = false;
    this.terminal?.resume();
  }

  async getForegroundProcess(): Promise<string> {
    await this.ready;
    const p = await this.terminal!.processes();
    return p.foreground_name || this.process;
  }
}

/** node-pty spawn(file, args, options). */
export function spawn(file: string, args: string[] | string = [], options: IPtyForkOptions = {}): IPty {
  return new BosonPty(file, args, options);
}

/** node-pty's default export shape. */
export const nodePty = { spawn };
export default nodePty;
