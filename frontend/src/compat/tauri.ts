/**
 * Tauri v2 compatibility shim: invoke() from @tauri-apps/api/core and listen / once / emit / emitTo from
 * @tauri-apps/api/event, on top of Boson IPC.
 *
 * Command mapping:
 *   invoke('Controller.method', args)  -> boson.call('Controller.method', args)        (static Boson controller)
 *   invoke('greet', args)              -> ElectronIpc.invoke { channel: 'greet', args: [args ?? {}] }
 *                                         (register with ElectronIpc::handle("greet", ...); args[0] is the
 *                                          named-arguments object, as in a #[tauri::command])
 *   mapCommand('greet', 'App.greet')   -> overrides the route for one command.
 * Events: listen(name) receives Bridge::emit(name, payload); emit(name, payload) reaches C++ listeners
 * (ElectronIpc::on_event) and is re-broadcast to every attached WebView.
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';

export type InvokeArgs = Record<string, unknown> | unknown[] | ArrayBuffer | Uint8Array;

export interface InvokeOptions {
  headers?: HeadersInit | Record<string, string>;
}

const commandRoutes = new Map<string, string>();

/** Routes a Tauri command name to a static Boson "Controller.method". */
export function mapCommand(command: string, bosonMethod: string): void {
  commandRoutes.set(command, bosonMethod);
}

export function invoke<T = unknown>(cmd: string, args?: InvokeArgs, _options?: InvokeOptions): Promise<T> {
  if (args instanceof ArrayBuffer || args instanceof Uint8Array) {
    return Promise.reject(new BosonIpcError('Binary invoke payloads are not supported; send base64 or a number[]', -32602));
  }
  if (cmd.startsWith('plugin:')) {
    const routed = commandRoutes.get(cmd);
    if (!routed) {
      return Promise.reject(new BosonIpcError(`Tauri plugin command '${cmd}' has no Boson mapping (see docs/MIGRATION.md)`, -32601));
    }
    return boson.call<T, unknown>(routed, args ?? {});
  }
  const method = commandRoutes.get(cmd) ?? (cmd.includes('.') ? cmd : undefined);
  if (method) {
    return boson.call<T, unknown>(method, args ?? {});
  }
  return boson.call<T, { channel: string; args: unknown[] }>('ElectronIpc.invoke', { channel: cmd, args: [args ?? {}] });
}

/** Tauri's isTauri(): false, so code can branch on Boson. */
export function isTauri(): boolean {
  return false;
}

// ---- @tauri-apps/api/event ---------------------------------------------------------------------------------

export type EventName = string;
export type UnlistenFn = () => void;

export interface Event<T> {
  event: EventName;
  /** Listener-local sequence number (Tauri: event id). */
  id: number;
  payload: T;
}

export type EventCallback<T> = (event: Event<T>) => void;

export type EventTarget =
  | string
  | { kind: 'Any' }
  | { kind: 'AnyLabel'; label: string }
  | { kind: 'App' }
  | { kind: 'Window'; label: string }
  | { kind: 'Webview'; label: string }
  | { kind: 'WebviewWindow'; label: string };

export interface Options {
  /** Ignored: every Boson event reaches every listener of the WebView it was sent to. */
  target?: EventTarget;
}

let nextEventId = 1;

export function listen<T>(event: EventName, handler: EventCallback<T>, _options?: Options): Promise<UnlistenFn> {
  const off: Unsubscribe = boson.on<T>(event, (payload: T) => {
    handler({ event, id: nextEventId++, payload });
  });
  return Promise.resolve(off);
}

export function once<T>(event: EventName, handler: EventCallback<T>, _options?: Options): Promise<UnlistenFn> {
  const off: Unsubscribe = boson.once<T>(event, (payload: T) => {
    handler({ event, id: nextEventId++, payload });
  });
  return Promise.resolve(off);
}

/** Delivers to C++ ElectronIpc::on_event listeners and to every attached WebView (including this one). */
export async function emit<T>(event: string, payload?: T): Promise<void> {
  await boson.call('ElectronIpc.emit', { event, payload: payload === undefined ? null : payload, broadcast: true });
}

/**
 * Boson has no window labels at the IPC layer: emitTo behaves like emit(). Target a specific WebView from C++
 * with Bridge::emit_to(webview_id, ...) instead.
 */
export async function emitTo<T>(_target: EventTarget, event: string, payload?: T): Promise<void> {
  await emit(event, payload);
}

export const core = { invoke, isTauri, mapCommand };
export const eventApi = { listen, once, emit, emitTo };
