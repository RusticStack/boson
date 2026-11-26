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
 * (ElectronIpc::on_event) and is re-broadcast to every attached WebView. emitTo(label, ...) goes to one window
 * (Window.emit, needs app::WindowManager).
 * Windows (@tauri-apps/api/window, /webviewWindow): Window, WebviewWindow, getCurrentWindow, getAllWindows on
 * the Window controller; see docs/WINDOWS.md.
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';
import { AppWindow, currentLabel, windows, type WindowInfo, type WindowOptions } from '../windows';

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

function targetLabel(target: EventTarget): string | null {
  if (typeof target === 'string') {
    return target;
  }
  return 'label' in target ? target.label : null;
}

/**
 * Window targets ('label', {kind:'Window'|'Webview'|'WebviewWindow'|'AnyLabel', label}) go to that window only
 * (Window.emit); {kind:'Any'} / {kind:'App'} behave like emit().
 */
export async function emitTo<T>(target: EventTarget, event: string, payload?: T): Promise<void> {
  const label = targetLabel(target);
  if (label === null) {
    await emit(event, payload);
    return;
  }
  await windows.emitTo(label, event, payload);
}

export const core = { invoke, isTauri, mapCommand };
export const eventApi = { listen, once, emit, emitTo };

// ---- @tauri-apps/api/dpi -------------------------------------------------------------------------------------

export class LogicalSize {
  readonly type = 'Logical';
  constructor(public width: number, public height: number) {}
  toPhysical(scaleFactor: number): PhysicalSize {
    return new PhysicalSize(Math.round(this.width * scaleFactor), Math.round(this.height * scaleFactor));
  }
}

export class PhysicalSize {
  readonly type = 'Physical';
  constructor(public width: number, public height: number) {}
  toLogical(scaleFactor: number): LogicalSize {
    return new LogicalSize(this.width / scaleFactor, this.height / scaleFactor);
  }
}

export class LogicalPosition {
  readonly type = 'Logical';
  constructor(public x: number, public y: number) {}
  toPhysical(scaleFactor: number): PhysicalPosition {
    return new PhysicalPosition(Math.round(this.x * scaleFactor), Math.round(this.y * scaleFactor));
  }
}

export class PhysicalPosition {
  readonly type = 'Physical';
  constructor(public x: number, public y: number) {}
  toLogical(scaleFactor: number): LogicalPosition {
    return new LogicalPosition(this.x / scaleFactor, this.y / scaleFactor);
  }
}

// ---- @tauri-apps/api/window + webviewWindow -------------------------------------------------------------------

export interface CloseRequestedEvent {
  event: 'tauri://close-requested';
  id: number;
  windowLabel: string;
  preventDefault(): void;
  isPreventDefault(): boolean;
}

/** Tauri WindowOptions / WebviewOptions subset Boson understands. */
export interface WebviewWindowOptions {
  url?: string;
  title?: string;
  width?: number;
  height?: number;
  minWidth?: number;
  minHeight?: number;
  x?: number;
  y?: number;
  center?: boolean;
  decorations?: boolean;
  transparent?: boolean;
  alwaysOnTop?: boolean;
  skipTaskbar?: boolean;
  visible?: boolean;
  focus?: boolean;
  /** Label (or Window) of the owner. */
  parent?: string | TauriWindow;
  /** Boson extension: disable the parent while open. */
  modal?: boolean;
  theme?: 'light' | 'dark';
}

function toBosonOptions(o: WebviewWindowOptions): WindowOptions {
  return {
    url: o.url,
    title: o.title,
    width: o.width,
    height: o.height,
    minWidth: o.minWidth,
    minHeight: o.minHeight,
    x: o.x,
    y: o.y,
    center: o.center,
    frameless: o.decorations === false ? true : undefined,
    transparent: o.transparent,
    alwaysOnTop: o.alwaysOnTop,
    skipTaskbar: o.skipTaskbar,
    visible: o.visible,
    focus: o.focus,
    parent: typeof o.parent === 'string' ? o.parent : o.parent?.label,
    modal: o.modal,
    darkMode: o.theme === undefined ? undefined : o.theme === 'dark',
  };
}

/** Tauri's Window class (named TauriWindow here so it does not shadow the DOM Window type). */
export class TauriWindow {
  protected readonly handle: AppWindow;
  constructor(public readonly label: string) {
    this.handle = new AppWindow(label);
  }

  static getCurrent(): TauriWindow {
    return new TauriWindow(currentLabel() ?? '');
  }
  static async getAll(): Promise<TauriWindow[]> {
    return (await windows.list()).map((w) => new TauriWindow(w.label));
  }
  static async getByLabel(label: string): Promise<TauriWindow | null> {
    return (await windows.get(label)) ? new TauriWindow(label) : null;
  }
  static async getFocusedWindow(): Promise<TauriWindow | null> {
    const w = await windows.focused();
    return w ? new TauriWindow(w.label) : null;
  }

  private info(): Promise<WindowInfo> {
    return this.handle.info();
  }

  listen<T>(event: EventName, handler: EventCallback<T>): Promise<UnlistenFn> {
    return listen<T>(event, handler);
  }
  once<T>(event: EventName, handler: EventCallback<T>): Promise<UnlistenFn> {
    return once<T>(event, handler);
  }
  emit<T>(event: string, payload?: T): Promise<void> {
    return emit(event, payload);
  }
  emitTo<T>(target: EventTarget, event: string, payload?: T): Promise<void> {
    return emitTo(target, event, payload);
  }

  async scaleFactor(): Promise<number> {
    return (await this.info()).scale_factor;
  }
  async innerSize(): Promise<PhysicalSize> {
    return this.outerSize();
  }
  async outerSize(): Promise<PhysicalSize> {
    const i = await this.info();
    return new PhysicalSize(Math.round(i.width * i.scale_factor), Math.round(i.height * i.scale_factor));
  }
  async innerPosition(): Promise<PhysicalPosition> {
    return this.outerPosition();
  }
  async outerPosition(): Promise<PhysicalPosition> {
    const i = await this.info();
    return new PhysicalPosition(Math.round(i.x * i.scale_factor), Math.round(i.y * i.scale_factor));
  }
  async title(): Promise<string> {
    return (await this.info()).title;
  }
  async isMaximized(): Promise<boolean> {
    return (await this.info()).maximized;
  }
  async isMinimized(): Promise<boolean> {
    return (await this.info()).minimized;
  }
  async isFullscreen(): Promise<boolean> {
    return (await this.info()).fullscreen;
  }
  async isVisible(): Promise<boolean> {
    return (await this.info()).visible;
  }
  async isFocused(): Promise<boolean> {
    return (await this.info()).focused;
  }
  async isEnabled(): Promise<boolean> {
    return (await this.info()).enabled;
  }

  setTitle(title: string): Promise<void> {
    return this.handle.setTitle(title);
  }
  show(): Promise<void> {
    return this.handle.show(true);
  }
  hide(): Promise<void> {
    return this.handle.hide();
  }
  close(): Promise<void> {
    return this.handle.close();
  }
  destroy(): Promise<void> {
    return this.handle.destroy();
  }
  setFocus(): Promise<void> {
    return this.handle.focus();
  }
  minimize(): Promise<void> {
    return this.handle.minimize();
  }
  unminimize(): Promise<void> {
    return this.handle.restore();
  }
  maximize(): Promise<void> {
    return this.handle.maximize();
  }
  unmaximize(): Promise<void> {
    return this.handle.unmaximize();
  }
  toggleMaximize(): Promise<void> {
    return this.handle.toggleMaximize();
  }
  center(): Promise<void> {
    return this.handle.center();
  }
  setAlwaysOnTop(alwaysOnTop: boolean): Promise<void> {
    return this.handle.setAlwaysOnTop(alwaysOnTop);
  }
  async setSkipTaskbar(skip: boolean): Promise<void> {
    await this.handle.setSkipTaskbar(skip);
  }
  setFullscreen(fullscreen: boolean): Promise<void> {
    return this.handle.setFullscreen(fullscreen);
  }
  async setSize(size: LogicalSize | PhysicalSize): Promise<void> {
    const logical = size instanceof PhysicalSize ? size.toLogical(await this.scaleFactor()) : size;
    await this.handle.setSize(Math.round(logical.width), Math.round(logical.height));
  }
  async setMinSize(size: LogicalSize | PhysicalSize | null): Promise<void> {
    if (size === null) {
      return this.handle.setMinSize(0, 0);
    }
    const logical = size instanceof PhysicalSize ? size.toLogical(await this.scaleFactor()) : size;
    return this.handle.setMinSize(Math.round(logical.width), Math.round(logical.height));
  }
  async setPosition(position: LogicalPosition | PhysicalPosition): Promise<void> {
    const logical = position instanceof PhysicalPosition ? position.toLogical(await this.scaleFactor()) : position;
    await this.handle.setPosition(Math.round(logical.x), Math.round(logical.y));
  }

  /** Current window only. preventDefault() keeps it open (also after an await). */
  onCloseRequested(handler: (event: CloseRequestedEvent) => void | Promise<void>): Promise<UnlistenFn> {
    return this.handle.onCloseRequested(async (request) => {
      await handler({
        event: 'tauri://close-requested',
        id: nextEventId++,
        windowLabel: request.label,
        preventDefault: () => request.preventDefault(),
        isPreventDefault: () => request.defaultPrevented,
      });
    });
  }
  onMoved(handler: EventCallback<PhysicalPosition>): Promise<UnlistenFn> {
    return Promise.resolve(this.handle.on('moved', (e) => {
      handler({ event: 'tauri://move', id: nextEventId++, payload: new PhysicalPosition(Math.round(e.x * e.scale_factor), Math.round(e.y * e.scale_factor)) });
    }));
  }
  onResized(handler: EventCallback<PhysicalSize>): Promise<UnlistenFn> {
    return Promise.resolve(this.handle.on('resized', (e) => {
      handler({ event: 'tauri://resize', id: nextEventId++, payload: new PhysicalSize(Math.round(e.width * e.scale_factor), Math.round(e.height * e.scale_factor)) });
    }));
  }
  onFocusChanged(handler: EventCallback<boolean>): Promise<UnlistenFn> {
    const offFocus = this.handle.on('focus', () => handler({ event: 'tauri://focus', id: nextEventId++, payload: true }));
    const offBlur = this.handle.on('blur', () => handler({ event: 'tauri://blur', id: nextEventId++, payload: false }));
    return Promise.resolve(() => {
      offFocus();
      offBlur();
    });
  }
}

const EXISTING: unique symbol = Symbol('boson.existing-window');

/**
 * new WebviewWindow(label, options) creates the window; listen for 'tauri://created' / 'tauri://error' on it
 * (delivered locally, as in Tauri).
 */
export class WebviewWindow extends TauriWindow {
  private readonly local = new Map<string, Set<EventCallback<unknown>>>();
  private settled: { event: string; payload: unknown } | null = null;

  constructor(label: string, options: WebviewWindowOptions | typeof EXISTING = {}) {
    super(label);
    if (options === EXISTING) {
      return; // handle to an existing window (getByLabel / getCurrent)
    }
    windows.create(label, toBosonOptions(options)).then(
      () => this.fire('tauri://created', null),
      (err: unknown) => this.fire('tauri://error', err instanceof Error ? err.message : String(err)),
    );
  }

  static override getCurrent(): WebviewWindow {
    return new WebviewWindow(currentLabel() ?? '', EXISTING);
  }
  static override async getAll(): Promise<WebviewWindow[]> {
    return (await windows.list()).map((w) => new WebviewWindow(w.label, EXISTING));
  }
  static override async getByLabel(label: string): Promise<WebviewWindow | null> {
    return (await windows.get(label)) ? new WebviewWindow(label, EXISTING) : null;
  }

  private fire(event: string, payload: unknown): void {
    this.settled = { event, payload };
    for (const handler of [...(this.local.get(event) ?? [])]) {
      handler({ event, id: nextEventId++, payload });
    }
  }

  override listen<T>(event: EventName, handler: EventCallback<T>): Promise<UnlistenFn> {
    if (event !== 'tauri://created' && event !== 'tauri://error') {
      return super.listen(event, handler);
    }
    const set = this.local.get(event) ?? new Set<EventCallback<unknown>>();
    this.local.set(event, set);
    const h = handler as EventCallback<unknown>;
    set.add(h);
    if (this.settled?.event === event) {
      h({ event, id: nextEventId++, payload: this.settled.payload });
    }
    return Promise.resolve(() => {
      set.delete(h);
    });
  }

  override once<T>(event: EventName, handler: EventCallback<T>): Promise<UnlistenFn> {
    if (event !== 'tauri://created' && event !== 'tauri://error') {
      return super.once(event, handler);
    }
    let off: UnlistenFn = () => {};
    let fired = false;
    const p = this.listen<T>(event, (e) => {
      if (fired) {
        return;
      }
      fired = true;
      off();
      handler(e);
    });
    void p.then((unlisten) => {
      off = unlisten;
      if (fired) {
        unlisten();
      }
    });
    return p;
  }
}

export function getCurrentWindow(): TauriWindow {
  return TauriWindow.getCurrent();
}
export function getAllWindows(): Promise<TauriWindow[]> {
  return TauriWindow.getAll();
}
export function getCurrentWebviewWindow(): WebviewWindow {
  return WebviewWindow.getCurrent();
}
export function getAllWebviewWindows(): Promise<WebviewWindow[]> {
  return WebviewWindow.getAll();
}

export const windowApi = {
  Window: TauriWindow,
  getCurrentWindow,
  getAllWindows,
  LogicalSize,
  PhysicalSize,
  LogicalPosition,
  PhysicalPosition,
};
export const webviewWindowApi = { WebviewWindow, getCurrentWebviewWindow, getAllWebviewWindows };
