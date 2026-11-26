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
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';
import { app as nativeApp, dialog as nativeDialog, zoom as nativeZoom } from '../native';

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

/**
 * Main-process APIs Electron apps usually reach over IPC, callable directly from the renderer in Boson.
 * dialog.* resolve with Electron's result shapes; the window argument of Electron's signatures is optional
 * and ignored (dialogs are owned by the Boson window).
 */
export const dialog = {
  showOpenDialog: (...args: unknown[]) => nativeDialog.showOpenDialog(lastObject(args)),
  showSaveDialog: (...args: unknown[]) => nativeDialog.showSaveDialog(lastObject(args)),
  showMessageBox: (...args: unknown[]) =>
    nativeDialog.showMessageBox(lastObject(args) as unknown as Parameters<typeof nativeDialog.showMessageBox>[0]),
  showErrorBox: (title: string, content: string) => nativeDialog.showErrorBox(title, content),
};

function lastObject(args: unknown[]): Record<string, never> {
  const found = [...args].reverse().find((a) => typeof a === 'object' && a !== null && !('hwnd' in (a as object)));
  return (found ?? {}) as Record<string, never>;
}

/** app.getAppMetrics / getLocale / getPreferredSystemLanguages / getSystemLocale (all async in Boson). */
export const app = {
  getAppMetrics: () => nativeApp.getAppMetrics(),
  getLocale: () => nativeApp.getLocale(),
  getSystemLocale: () => nativeApp.getSystemLocale(),
  getPreferredSystemLanguages: () => nativeApp.getPreferredSystemLanguages(),
};

/** systemPreferences.getAccentColor() resolves "rrggbbaa" like Electron. */
export const systemPreferences = {
  getAccentColor: async () => (await nativeApp.systemPreferences()).accent_color.replace('#', '') + 'ff',
  getAnimationSettings: async () => {
    const p = await nativeApp.systemPreferences();
    return { shouldRenderRichAnimation: !p.reduced_motion, scrollAnimationsEnabledBySystem: !p.reduced_motion, prefersReducedMotion: p.reduced_motion };
  },
};

let cachedZoomFactor = 1;
nativeZoom.onChanged((s) => {
  cachedZoomFactor = s.factor;
});

/** webFrame zoom: setters are fire-and-forget, getters return the last known value (zoom:changed). */
export const webFrame = {
  setZoomFactor: (factor: number) => {
    cachedZoomFactor = factor;
    void nativeZoom.set(factor).catch(() => {});
  },
  getZoomFactor: () => cachedZoomFactor,
  setZoomLevel: (level: number) => {
    cachedZoomFactor = Math.pow(1.2, level);
    void nativeZoom.setLevel(level).catch(() => {});
  },
  getZoomLevel: () => Math.log(cachedZoomFactor) / Math.log(1.2),
};

/** Drop-in for `import { ipcRenderer, contextBridge } from 'electron'` via a bundler alias. */
export const electron = { ipcRenderer, contextBridge, dialog, app, systemPreferences, webFrame };
export default electron;
