/**
 * Electron renderer compatibility shim: ipcRenderer and contextBridge shapes on top of Boson IPC.
 *
 * Channel convention (C++ side: boson::compat::ElectronIpc, registered in the app's Router):
 *   ipcRenderer.invoke(ch, ...args)  -> boson.call('ElectronIpc.invoke', { channel: ch, args })   (ipcMain.handle)
 *   ipcRenderer.send(ch, ...args)    -> boson.call('ElectronIpc.send',   { channel: ch, args })   (ipcMain.on)
 *   ipcRenderer.on(ch, listener)     <- Bridge event "ipc:<ch>" whose data is the args array    (webContents.send / event.reply)
 *   ipcRenderer.sendSync(ch, ...a)   -> boot value sendSync[ch] (no args) or
 *                                       boson.invokeSync('ElectronIpc.sendSync', { channel, args }) (ElectronIpc::handle_sync)
 *
 * Arguments travel as JSON, not structured clone: Date, Map, Set, typed arrays, MessagePort and functions are
 * not supported (convert them first). postMessage with transferables is not supported.
 *
 * Also here: webUtils.getPathForFile, webFrame zoom, a process stub, clipboard (sync, via the Clipboard
 * controller) and startDrag (webContents.startDrag). See docs/SYNC_IPC.md and docs/PORTING.md.
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';
import { startDrag as nativeStartDrag, type DragResult } from '../dragout';

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

  /**
   * Synchronous request (Electron: ipcMain.on + event.returnValue). Without arguments the reply is first looked up
   * in the boot values (C++ BootValues::set_send_sync, no IPC at all); otherwise it goes through the SyncBridge
   * host object to ElectronIpc::handle_sync(channel) on the UI thread. Blocks this page until the reply arrives.
   * Throws BosonIpcError when no SyncBridge is attached or no sync handler is registered.
   */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  public sendSync(channel: string, ...args: unknown[]): any {
    if (args.length === 0) {
      const replies = boson.boot<Record<string, unknown>>('sendSync');
      if (replies && Object.prototype.hasOwnProperty.call(replies, channel)) {
        return cloneJson(replies[channel]); // Electron returns a fresh copy; boot values are frozen
      }
    }
    return boson.invokeSync('ElectronIpc.sendSync', { channel, args });
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

function cloneJson<T>(value: T): T {
  return value === undefined || value === null || typeof value !== 'object' ? value : (JSON.parse(JSON.stringify(value)) as T);
}

/**
 * contextBridge.exposeInMainWorld: Boson pages run in a single JS world (WebView2 has no preload isolated
 * world), so this defines a frozen, non-writable global instead of crossing an isolation boundary. Differences:
 * the page's own scripts share the world with the "preload" code, functions are passed by reference (not
 * proxied), and objects are frozen shallowly. Treat it as an API-shape shim, not a security boundary; the
 * security boundary is the Bridge origin check + CapabilityPolicy on the C++ side.
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

// ---- webUtils -----------------------------------------------------------------------------------------------

let nextFileToken = 1;
const filePaths = new WeakMap<File, string>();

/**
 * webUtils.getPathForFile: the real path of a File from a drop or an <input type=file>. The File is posted to
 * the host with postMessageWithAdditionalObjects (WebView2 resolves it to an ICoreWebView2File path), then the
 * path is read back synchronously from SyncHost.file_paths. '' for files that have no path (built in script).
 */
export const webUtils = {
  getPathForFile(file: File): string {
    const cached = filePaths.get(file);
    if (cached !== undefined) {
      return cached;
    }
    const webview = window.chrome?.webview;
    if (!webview || typeof webview.postMessageWithAdditionalObjects !== 'function') {
      throw new BosonIpcError('webUtils.getPathForFile needs WebView2 postMessageWithAdditionalObjects', -32001);
    }
    const token = nextFileToken++;
    try {
      webview.postMessageWithAdditionalObjects({ __boson_sync: 'files', token }, [file]);
    } catch {
      // WebView2 rejects File objects that are not backed by a file on disk (built in script): Electron returns ''.
      filePaths.set(file, '');
      return '';
    }
    const { paths } = boson.invokeSync<{ paths: string[] }, { token: number }>('SyncHost.file_paths', { token });
    const path = paths[0] ?? '';
    filePaths.set(file, path);
    return path;
  },
};

// ---- webFrame -----------------------------------------------------------------------------------------------

/** webFrame zoom (the WebView's ICoreWebView2Controller ZoomFactor), synchronous like Electron's. */
export const webFrame = {
  getZoomFactor(): number {
    return boson.invokeSync<{ value: number }>('SyncHost.zoom_factor').value;
  },
  setZoomFactor(factor: number): void {
    boson.invokeSync('SyncHost.set_zoom_factor', { value: factor });
  },
  /** Electron zoom level: factor = 1.2 ^ level. */
  getZoomLevel(): number {
    return Math.log(webFrame.getZoomFactor()) / Math.log(1.2);
  },
  setZoomLevel(level: number): void {
    webFrame.setZoomFactor(Math.pow(1.2, level));
  },
};

// ---- process ------------------------------------------------------------------------------------------------

interface BootProcessInfo {
  platform?: string;
  arch?: string;
  versions?: Record<string, string>;
}

function userAgentVersions(): Record<string, string> {
  const ua = typeof navigator === 'undefined' ? '' : navigator.userAgent;
  const out: Record<string, string> = {};
  const chrome = /Chrome\/([\d.]+)/.exec(ua);
  const edge = /Edg\/([\d.]+)/.exec(ua);
  if (chrome) out.chrome = chrome[1];
  if (edge) out.webview2 = edge[1];
  return out;
}

const bootProcess = boson.boot<BootProcessInfo>('process');

/**
 * Renderer-side stand-in for Node's process: platform / arch / versions (from BootValues::set_process_info,
 * else the user agent), an empty env and no Node APIs. versions.electron and versions.node are absent.
 */
export const process = Object.freeze({
  platform: 'win32' as const,
  arch: bootProcess?.arch ?? 'x64',
  versions: Object.freeze({ ...userAgentVersions(), ...(bootProcess?.versions ?? {}) }) as Readonly<Record<string, string>>,
  type: 'renderer' as const,
  env: Object.freeze({}) as Readonly<Record<string, string | undefined>>,
  argv: Object.freeze([]) as readonly string[],
  sandboxed: true,
  contextIsolated: false,
  cwd: (): string => '',
});

/** Defines window.process (when absent) for bundles that read process.platform / process.env at runtime. */
export function installProcessGlobal(): void {
  const global = window as unknown as Record<string, unknown>;
  if (global.process === undefined) {
    Object.defineProperty(window, 'process', { value: process, writable: false, configurable: true, enumerable: false });
  }
}

// ---- clipboard ----------------------------------------------------------------------------------------------

/**
 * Electron's synchronous clipboard over the Clipboard controller. Needs a SyncBridge whose sync methods include
 * the Clipboard methods used (e.g. "Clipboard.*"); each call opens the clipboard on the UI thread.
 */
export const clipboard = {
  readText(): string {
    return boson.invokeSync<{ text: string | null }>('Clipboard.read_text').text ?? '';
  },
  writeText(text: string): void {
    boson.invokeSync('Clipboard.write_text', { text });
  },
  readHTML(): string {
    const html = boson.invokeSync<{ available: boolean; html: string; fragment: string }>('Clipboard.read_html');
    return html.available ? html.fragment || html.html : '';
  },
  writeHTML(markup: string): void {
    boson.invokeSync('Clipboard.write_html', { html: markup });
  },
  write(data: { text?: string; html?: string }): void {
    if (data.html !== undefined) {
      const request: { html: string; text?: string } = data.text === undefined ? { html: data.html } : { html: data.html, text: data.text };
      boson.invokeSync<unknown, { html: string; text?: string }>('Clipboard.write_html', request);
    } else {
      clipboard.writeText(data.text ?? '');
    }
  },
  clear(): void {
    boson.invokeSync('Clipboard.clear');
  },
  availableFormats(): string[] {
    return boson.invokeSync<{ formats: Array<{ name: string }> }>('Clipboard.formats').formats.map((f) => f.name);
  },
  has(format: string): boolean {
    return boson.invokeSync<{ value: boolean }, { format: string }>('Clipboard.has', { format }).value;
  },
};

// ---- webContents.startDrag ------------------------------------------------------------------------------------

/** Electron's DragItem. icon is a path to an image file (NativeImage is not available in the renderer). */
export interface DragItem {
  file?: string;
  files?: string[];
  icon?: string;
}

/**
 * webContents.startDrag(item) from the renderer (Electron calls it in the main process on event.sender). Call it
 * from a dragstart handler after event.preventDefault(). Resolves with the drop effect when the drag ends.
 */
export function startDrag(item: DragItem): Promise<DragResult> {
  const files = item.files ?? (item.file !== undefined ? [item.file] : []);
  return nativeStartDrag(files, item.icon !== undefined ? { icon: item.icon } : {});
}

/** Drop-in for `import { ipcRenderer, contextBridge, ... } from 'electron'` via a bundler alias. */
export const electron = { ipcRenderer, contextBridge, webUtils, webFrame, process, clipboard, startDrag };
export default electron;
