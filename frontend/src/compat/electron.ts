/**
 * Electron renderer compatibility shim: ipcRenderer and contextBridge shapes on top of Boson IPC.
 *
 * Channel convention (C++ side: boson::compat::ElectronIpc, registered in the app's Router):
 *   ipcRenderer.invoke(ch, ...args)  -> boson.call('ElectronIpc.invoke', { channel: ch, args })   (ipcMain.handle)
 *   ipcRenderer.send(ch, ...args)    -> boson.call('ElectronIpc.send',   { channel: ch, args })   (ipcMain.on)
 *   ipcRenderer.on(ch, listener)     <- Bridge event "ipc:<ch>" whose data is the args array    (webContents.send / event.reply)
 *
 * Arguments travel as JSON, not structured clone: Date, Map, Set, typed arrays, MessagePort and functions are
 * not supported (convert them first). sendSync / postMessage with transferables are not supported.
 *
 * BrowserWindow: a renderer-side proxy (like @electron/remote) over the Window controller. Getters that are
 * synchronous in Electron (getBounds, isMaximized, getAllWindows...) return Promises here.
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';
import { AppWindow, currentLabel, windows, type WindowInfo } from '../windows';

export const IPC_EVENT_PREFIX = 'ipc:';

/** Event bound to channel listeners (Electron's IpcRendererEvent, minus ports/senderFrame). */
export interface IpcRendererEvent {
  sender: IpcRenderer;
  channel: string;
  ports: never[];
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type IpcRendererListener = (event: IpcRendererEvent, ...args: any[]) => void;

export class IpcRenderer {
  private readonly subscriptions = new Map<string, Array<{ listener: IpcRendererListener; off: Unsubscribe }>>();

  /** ipcMain.handle(channel) on the C++ side (ElectronIpc::handle). Rejects with BosonIpcError. */
  public invoke<T = unknown>(channel: string, ...args: unknown[]): Promise<T> {
    return boson.call<T, { channel: string; args: unknown[] }>('ElectronIpc.invoke', { channel, args });
  }

  /** Fire-and-forget to ipcMain.on(channel) listeners (ElectronIpc::on). */
  public send(channel: string, ...args: unknown[]): void {
    boson.call('ElectronIpc.send', { channel, args }).catch((err: unknown) => {
      console.error(`[boson/electron] ipcRenderer.send('${channel}') failed`, err);
    });
  }

  /** Not supported: the WebView2 bridge is asynchronous by design (the UI thread never blocks). */
  public sendSync(channel: string): never {
    throw new BosonIpcError(`ipcRenderer.sendSync('${channel}') is not supported by Boson; use invoke()`, -32601);
  }

  /** Same as send(): Boson has a single top-level frame per WebView. */
  public sendToHost(channel: string, ...args: unknown[]): void {
    this.send(channel, ...args);
  }

  public on(channel: string, listener: IpcRendererListener): this {
    this.add(channel, listener, false);
    return this;
  }

  public once(channel: string, listener: IpcRendererListener): this {
    this.add(channel, listener, true);
    return this;
  }

  public addListener(channel: string, listener: IpcRendererListener): this {
    return this.on(channel, listener);
  }

  /** Removes the most recently added registration of listener (Node EventEmitter semantics). */
  public removeListener(channel: string, listener: IpcRendererListener): this {
    const list = this.subscriptions.get(channel);
    if (!list) {
      return this;
    }
    for (let i = list.length - 1; i >= 0; i--) {
      if (list[i].listener === listener) {
        list[i].off();
        list.splice(i, 1);
        break;
      }
    }
    if (list.length === 0) {
      this.subscriptions.delete(channel);
    }
    return this;
  }

  public off(channel: string, listener: IpcRendererListener): this {
    return this.removeListener(channel, listener);
  }

  /** Without a channel, removes every listener of every channel. */
  public removeAllListeners(channel?: string): this {
    const channels = channel === undefined ? [...this.subscriptions.keys()] : [channel];
    for (const name of channels) {
      for (const entry of this.subscriptions.get(name) ?? []) {
        entry.off();
      }
      this.subscriptions.delete(name);
    }
    return this;
  }

  public listenerCount(channel: string): number {
    return this.subscriptions.get(channel)?.length ?? 0;
  }

  public eventNames(): string[] {
    return [...this.subscriptions.keys()];
  }

  private add(channel: string, listener: IpcRendererListener, once: boolean): void {
    const event: IpcRendererEvent = { sender: this, channel, ports: [] };
    let list = this.subscriptions.get(channel);
    if (!list) {
      list = [];
      this.subscriptions.set(channel, list);
    }
    const entry = { listener, off: (() => {}) as Unsubscribe };
    entry.off = boson.on<unknown>(IPC_EVENT_PREFIX + channel, (data: unknown) => {
      if (once) {
        this.removeEntry(channel, entry);
      }
      const args = Array.isArray(data) ? data : [data];
      listener(event, ...args);
    });
    list.push(entry);
  }

  private removeEntry(channel: string, entry: { listener: IpcRendererListener; off: Unsubscribe }): void {
    const list = this.subscriptions.get(channel);
    const index = list?.indexOf(entry) ?? -1;
    if (list && index >= 0) {
      entry.off();
      list.splice(index, 1);
      if (list.length === 0) {
        this.subscriptions.delete(channel);
      }
    }
  }
}

export const ipcRenderer = new IpcRenderer();

/**
 * contextBridge.exposeInMainWorld: Boson pages run in a single JS world, so this defines a frozen,
 * non-writable global instead of crossing an isolation boundary. Preload-style code can keep calling it.
 */
export const contextBridge = {
  exposeInMainWorld(apiKey: string, api: unknown): void {
    if (Object.prototype.hasOwnProperty.call(window, apiKey)) {
      throw new Error(`contextBridge: window.${apiKey} is already defined`);
    }
    const value = typeof api === 'object' && api !== null ? Object.freeze(api) : api;
    Object.defineProperty(window, apiKey, { value, writable: false, configurable: false, enumerable: true });
  },
};

// ---- BrowserWindow (renderer-side proxy) ---------------------------------------------------------------------

export interface Rectangle {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Electron BrowserWindowConstructorOptions subset, plus Boson's label / url. */
export interface BrowserWindowConstructorOptions {
  /** Boson: window label (default "window-<n>"). */
  label?: string;
  /** Boson: first URL (Electron apps call loadURL / loadFile afterwards, which also works). */
  url?: string;
  width?: number;
  height?: number;
  minWidth?: number;
  minHeight?: number;
  x?: number;
  y?: number;
  center?: boolean;
  title?: string;
  show?: boolean;
  frame?: boolean;
  titleBarStyle?: 'default' | 'hidden' | 'hiddenInset';
  transparent?: boolean;
  alwaysOnTop?: boolean;
  skipTaskbar?: boolean;
  parent?: BrowserWindow;
  modal?: boolean;
  backgroundMaterial?: 'auto' | 'none' | 'mica' | 'acrylic' | 'tabbed';
}

export type BrowserWindowEvent =
  | 'close' | 'closed' | 'focus' | 'blur' | 'move' | 'resize' | 'maximize' | 'unmaximize' | 'minimize' | 'restore'
  | 'show' | 'hide';

export interface BrowserWindowCloseEvent {
  preventDefault(): void;
  readonly defaultPrevented: boolean;
}

const bosonEventOf: Record<Exclude<BrowserWindowEvent, 'close' | 'unmaximize'>, string> = {
  closed: 'closed',
  focus: 'focus',
  blur: 'blur',
  move: 'moved',
  resize: 'resized',
  maximize: 'maximized',
  minimize: 'minimized',
  restore: 'restored',
  show: 'shown',
  hide: 'hidden',
};

let windowCounter = 0;
const numericIds = new Map<string, number>();
function idOf(label: string): number {
  let id = numericIds.get(label);
  if (id === undefined) {
    id = numericIds.size + 1;
    numericIds.set(label, id);
  }
  return id;
}

export class BrowserWindow {
  /** Resolves once the window exists (rejects when Window.create failed). */
  public readonly ready: Promise<void>;
  public readonly label: string;
  public readonly id: number;
  private readonly handle: AppWindow;
  private readonly offs = new Map<(...args: never[]) => void, Unsubscribe[]>();

  /** Creates a window. BrowserWindow.fromLabel() returns a proxy for an existing one instead. */
  constructor(options: BrowserWindowConstructorOptions = {}, existing = false) {
    this.label = options.label ?? (existing ? '' : `window-${Date.now().toString(36)}-${++windowCounter}`);
    this.id = idOf(this.label);
    this.handle = new AppWindow(this.label);
    if (existing) {
      this.ready = Promise.resolve();
      return;
    }
    const material = options.backgroundMaterial;
    this.ready = windows
      .create(this.label, {
        url: options.url ?? '',
        title: options.title,
        width: options.width,
        height: options.height,
        minWidth: options.minWidth,
        minHeight: options.minHeight,
        x: options.x,
        y: options.y,
        center: options.center,
        visible: options.show ?? true,
        frameless: options.frame === false ? true : undefined,
        headlessTitlebar: options.titleBarStyle === 'hidden' || options.titleBarStyle === 'hiddenInset' ? true : undefined,
        transparent: options.transparent,
        alwaysOnTop: options.alwaysOnTop,
        skipTaskbar: options.skipTaskbar,
        parent: options.parent?.label,
        modal: options.modal,
        backdrop: material === 'tabbed' ? 'mica_alt' : material === 'auto' ? undefined : material,
      })
      .then(() => undefined);
  }

  static fromLabel(label: string): BrowserWindow {
    return new BrowserWindow({ label }, true);
  }
  /** The window of this renderer (@electron/remote getCurrentWindow()). */
  static getCurrentWindow(): BrowserWindow {
    return BrowserWindow.fromLabel(currentLabel() ?? '');
  }
  static async getAllWindows(): Promise<BrowserWindow[]> {
    return (await windows.list()).map((w) => BrowserWindow.fromLabel(w.label));
  }
  static async getFocusedWindow(): Promise<BrowserWindow | null> {
    const w = await windows.focused();
    return w ? BrowserWindow.fromLabel(w.label) : null;
  }
  static async fromId(id: number): Promise<BrowserWindow | null> {
    for (const [label, value] of numericIds) {
      if (value === id && (await windows.get(label))) {
        return BrowserWindow.fromLabel(label);
      }
    }
    return null;
  }

  private async after<T>(fn: () => Promise<T>): Promise<T> {
    await this.ready;
    return fn();
  }
  private info(): Promise<WindowInfo> {
    return this.after(() => this.handle.info());
  }

  /** Electron webContents subset: send(channel, ...args) reaches ipcRenderer.on(channel) in that window. */
  public readonly webContents = {
    send: (channel: string, ...args: unknown[]): void => {
      void this.after(() => this.handle.emit(IPC_EVENT_PREFIX + channel, args)).catch((err: unknown) => {
        console.error(`[boson/electron] webContents.send('${channel}') failed`, err);
      });
    },
  };

  loadURL(url: string): Promise<void> {
    return this.after(() => this.handle.navigate(url));
  }
  loadFile(path: string): Promise<void> {
    return this.after(() => this.handle.navigate(path));
  }
  setTitle(title: string): Promise<void> {
    return this.after(() => this.handle.setTitle(title));
  }
  async getTitle(): Promise<string> {
    return (await this.info()).title;
  }
  show(): Promise<void> {
    return this.after(() => this.handle.show(true));
  }
  showInactive(): Promise<void> {
    return this.after(() => this.handle.show(false));
  }
  hide(): Promise<void> {
    return this.after(() => this.handle.hide());
  }
  close(): Promise<void> {
    return this.after(() => this.handle.close());
  }
  destroy(): Promise<void> {
    return this.after(() => this.handle.destroy());
  }
  focus(): Promise<void> {
    return this.after(() => this.handle.focus());
  }
  minimize(): Promise<void> {
    return this.after(() => this.handle.minimize());
  }
  maximize(): Promise<void> {
    return this.after(() => this.handle.maximize());
  }
  unmaximize(): Promise<void> {
    return this.after(() => this.handle.unmaximize());
  }
  restore(): Promise<void> {
    return this.after(() => this.handle.restore());
  }
  center(): Promise<void> {
    return this.after(() => this.handle.center());
  }
  setAlwaysOnTop(flag: boolean): Promise<void> {
    return this.after(() => this.handle.setAlwaysOnTop(flag));
  }
  async setSkipTaskbar(skip: boolean): Promise<void> {
    await this.after(() => this.handle.setSkipTaskbar(skip));
  }
  setFullScreen(flag: boolean): Promise<void> {
    return this.after(() => this.handle.setFullscreen(flag));
  }
  setMinimumSize(width: number, height: number): Promise<void> {
    return this.after(() => this.handle.setMinSize(width, height));
  }
  async setBounds(bounds: Partial<Rectangle>): Promise<void> {
    await this.after(() => this.handle.setBounds(bounds));
  }
  async getBounds(): Promise<Rectangle> {
    const i = await this.info();
    return { x: i.x, y: i.y, width: i.width, height: i.height };
  }
  async setSize(width: number, height: number): Promise<void> {
    await this.after(() => this.handle.setSize(width, height));
  }
  async getSize(): Promise<[number, number]> {
    const i = await this.info();
    return [i.width, i.height];
  }
  async setPosition(x: number, y: number): Promise<void> {
    await this.after(() => this.handle.setPosition(x, y));
  }
  async getPosition(): Promise<[number, number]> {
    const i = await this.info();
    return [i.x, i.y];
  }
  async isMaximized(): Promise<boolean> {
    return (await this.info()).maximized;
  }
  async isMinimized(): Promise<boolean> {
    return (await this.info()).minimized;
  }
  async isFullScreen(): Promise<boolean> {
    return (await this.info()).fullscreen;
  }
  async isVisible(): Promise<boolean> {
    return (await this.info()).visible;
  }
  async isFocused(): Promise<boolean> {
    return (await this.info()).focused;
  }
  async isDestroyed(): Promise<boolean> {
    return (await windows.get(this.label)) === null;
  }

  /** 'close' (preventDefault() vetoes; current window only), 'closed', 'focus', 'blur', 'move', 'resize',
   * 'maximize', 'unmaximize', 'minimize', 'restore', 'show', 'hide'. */
  on(event: 'close', listener: (event: BrowserWindowCloseEvent) => void | Promise<void>): this;
  on(event: Exclude<BrowserWindowEvent, 'close'>, listener: () => void): this;
  on(event: BrowserWindowEvent, listener: (event: BrowserWindowCloseEvent) => void | Promise<void>): this {
    const list = this.offs.get(listener) ?? [];
    this.offs.set(listener, list);
    if (event === 'close') {
      void this.handle
        .onCloseRequested((request) => listener(request))
        .then((off) => list.push(off))
        .catch((err: unknown) => console.error('[boson/electron] BrowserWindow close listener', err));
      return this;
    }
    if (event === 'unmaximize') {
      // Electron fires 'unmaximize' when a maximized window is restored.
      let maximized = false;
      list.push(this.handle.on('maximized', () => { maximized = true; }));
      list.push(this.handle.on('restored', () => {
        if (maximized) {
          maximized = false;
          (listener as () => void)();
        }
      }));
      return this;
    }
    list.push(this.handle.on(bosonEventOf[event] as 'focus', () => (listener as () => void)()));
    return this;
  }

  once(event: Exclude<BrowserWindowEvent, 'close'>, listener: () => void): this {
    const wrapped = (): void => {
      this.off(event, wrapped);
      listener();
    };
    return this.on(event, wrapped);
  }

  off(_event: BrowserWindowEvent, listener: (...args: never[]) => void): this {
    for (const off of this.offs.get(listener) ?? []) {
      off();
    }
    this.offs.delete(listener);
    return this;
  }

  removeListener(event: BrowserWindowEvent, listener: (...args: never[]) => void): this {
    return this.off(event, listener);
  }
}

/** Drop-in for `import { ipcRenderer, contextBridge } from 'electron'` via a bundler alias. */
export const electron = { ipcRenderer, contextBridge, BrowserWindow };
export default electron;
