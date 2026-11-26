// Multi-window helpers over the Window controller (C++: boson::app::WindowManager + ipc::WindowController).
// Every window has a string label; the current one is injected as window.__BOSON_WINDOW__ before any page script,
// so AppWindow.current() is synchronous. Sizes and positions are DIPs.
import { boson, BosonIpcError, type Unsubscribe } from './boson';

export interface WindowInfo {
  label: string;
  title: string;
  url: string;
  x: number;
  y: number;
  width: number;
  height: number;
  scale_factor: number;
  visible: boolean;
  focused: boolean;
  enabled: boolean;
  maximized: boolean;
  minimized: boolean;
  fullscreen: boolean;
  modal: boolean;
  parent: string;
  close_guard: boolean;
  webview_ids: number[];
}

export type WindowBackdrop = 'none' | 'mica' | 'mica_alt' | 'acrylic' | 'transparent' | 'translucent';

export interface WindowOptions {
  /** '' / omitted = index.html; relative paths resolve against the dev server or boson://app/. */
  url?: string;
  title?: string;
  width?: number;
  height?: number;
  minWidth?: number;
  minHeight?: number;
  x?: number;
  y?: number;
  center?: boolean;
  /** No native frame at all. */
  frameless?: boolean;
  /** Native resize borders, HTML titlebar (CSS app-region: drag). */
  headlessTitlebar?: boolean;
  backdrop?: WindowBackdrop;
  darkMode?: boolean;
  alwaysOnTop?: boolean;
  skipTaskbar?: boolean;
  transparent?: boolean;
  visible?: boolean;
  focus?: boolean;
  /** Owner label: the new window stays above it and closes with it. */
  parent?: string;
  /** Disables the owner (parent, or the calling window) while open; centered on it. */
  modal?: boolean;
  persistState?: boolean;
  /** Closing hides the window instead (tray pattern). */
  hideOnClose?: boolean;
}

export interface WindowBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface WindowLabelEvent {
  label: string;
}

export interface WindowBoundsEvent extends WindowBounds {
  label: string;
  scale_factor: number;
}

export interface WindowCloseRequestedEvent {
  label: string;
  request_id: number;
}

export type WindowEventName =
  | 'created' | 'closed' | 'focus' | 'blur' | 'moved' | 'resized'
  | 'maximized' | 'minimized' | 'restored' | 'shown' | 'hidden';

export interface CloseRequest {
  readonly label: string;
  /** Keeps the window open. May be called after awaiting (the handler may be async). */
  preventDefault(): void;
  readonly defaultPrevented: boolean;
}

declare global {
  interface Window {
    __BOSON_WINDOW__?: { label: string };
  }
}

/** Label of this page's window, or null outside a WindowManager window. */
export function currentLabel(): string | null {
  return window.__BOSON_WINDOW__?.label ?? null;
}

const call = <R = void, P = unknown>(method: string, params: P): Promise<R> => boson.call<R, P>(method, params);

// ---- Close guard (only the window being closed receives window:close-requested) -----------------------------

type CloseHandler = (request: CloseRequest) => void | Promise<void>;
const closeHandlers = new Set<CloseHandler>();
let closeSubscription: Unsubscribe | null = null;

async function answerClose(e: WindowCloseRequestedEvent): Promise<void> {
  let prevented = false;
  const request: CloseRequest = {
    label: e.label,
    preventDefault() {
      prevented = true;
    },
    get defaultPrevented() {
      return prevented;
    },
  };
  for (const handler of [...closeHandlers]) {
    try {
      await handler(request);
    } catch (err) {
      console.error('[boson] close handler threw; keeping the window open', err);
      prevented = true;
    }
  }
  await call('Window.confirm_close', { label: '', request_id: e.request_id, allow: !prevented });
}

async function addCloseHandler(handler: CloseHandler): Promise<Unsubscribe> {
  closeHandlers.add(handler);
  if (!closeSubscription) {
    closeSubscription = boson.on<WindowCloseRequestedEvent>('window:close-requested', (e) => {
      void answerClose(e).catch((err: unknown) => console.error('[boson] confirm_close failed', err));
    });
    await call('Window.set_close_guard', { label: '', enabled: true });
  }
  return () => {
    closeHandlers.delete(handler);
    if (closeHandlers.size === 0 && closeSubscription) {
      closeSubscription();
      closeSubscription = null;
      void call('Window.set_close_guard', { label: '', enabled: false }).catch(() => {});
    }
  };
}

// ---- AppWindow --------------------------------------------------------------------------------------------

/** A window by label. Methods reject with BosonIpcError when the window no longer exists. */
export class AppWindow {
  constructor(public readonly label: string) {}

  /** This page's window (label '' = "the calling window" when no label was injected). */
  static current(): AppWindow {
    return new AppWindow(currentLabel() ?? '');
  }

  get isCurrent(): boolean {
    return this.label === '' || this.label === currentLabel();
  }

  private req<P extends object>(extra?: P): P & { label: string } {
    return { label: this.label, ...(extra ?? ({} as P)) };
  }

  info(): Promise<WindowInfo> {
    return call<WindowInfo>('Window.get', this.req());
  }
  show(focus = true): Promise<void> {
    return call('Window.show', this.req({ focus }));
  }
  hide(): Promise<void> {
    return call('Window.hide', this.req());
  }
  focus(): Promise<void> {
    return call('Window.focus', this.req());
  }
  /** Asks to close (close handlers may veto; hideOnClose windows hide). */
  close(): Promise<void> {
    return call('Window.close', this.req());
  }
  /** Closes without asking. */
  destroy(): Promise<void> {
    return call('Window.destroy', this.req());
  }
  navigate(url: string): Promise<void> {
    return call('Window.navigate', this.req({ url }));
  }
  minimize(): Promise<void> {
    return call('Window.minimize', this.req());
  }
  maximize(): Promise<void> {
    return call('Window.maximize', this.req());
  }
  unmaximize(): Promise<void> {
    return call('Window.unmaximize', this.req());
  }
  async toggleMaximize(): Promise<void> {
    return (await this.info()).maximized ? this.unmaximize() : this.maximize();
  }
  restore(): Promise<void> {
    return call('Window.restore', this.req());
  }
  center(): Promise<void> {
    return call('Window.center', this.req());
  }
  setTitle(title: string): Promise<void> {
    return call('Window.set_title', this.req({ title }));
  }
  setBounds(bounds: Partial<WindowBounds>): Promise<WindowInfo> {
    return call<WindowInfo>('Window.set_bounds', this.req(bounds));
  }
  setSize(width: number, height: number): Promise<WindowInfo> {
    return this.setBounds({ width, height });
  }
  setPosition(x: number, y: number): Promise<WindowInfo> {
    return this.setBounds({ x, y });
  }
  setMinSize(width: number, height: number): Promise<void> {
    return call('Window.set_min_size', this.req({ width, height }));
  }
  setAlwaysOnTop(enabled: boolean): Promise<void> {
    return call('Window.set_always_on_top', this.req({ enabled }));
  }
  setFullscreen(enabled: boolean): Promise<void> {
    return call('Window.set_fullscreen', this.req({ enabled }));
  }
  async setSkipTaskbar(enabled: boolean): Promise<boolean> {
    return (await call<{ ok: boolean }>('Window.set_skip_taskbar', this.req({ enabled }))).ok;
  }
  /** Sends an event to every WebView of this window. */
  async emit<T>(event: string, payload?: T): Promise<void> {
    await call('Window.emit', { target: this.label, event, payload: payload === undefined ? null : payload });
  }

  /** Window lifecycle / geometry events of this window (moved/resized reach other windows only when the C++
   * side enables WindowControllerOptions::broadcast_geometry). */
  on(event: 'moved' | 'resized', listener: (e: WindowBoundsEvent) => void): Unsubscribe;
  on(event: WindowEventName, listener: (e: WindowLabelEvent) => void): Unsubscribe;
  on(event: WindowEventName, listener: (e: WindowBoundsEvent) => void): Unsubscribe {
    return boson.on<WindowBoundsEvent>(`window:${event}`, (e) => {
      if (e.label === this.label || (this.label === '' && e.label === currentLabel())) {
        listener(e);
      }
    });
  }

  /** Vetoable close (Electron 'close' / Tauri onCloseRequested); current window only. */
  onCloseRequested(handler: CloseHandler): Promise<Unsubscribe> {
    if (!this.isCurrent) {
      return Promise.reject(new BosonIpcError('onCloseRequested works for the current window only', -32602));
    }
    return addCloseHandler(handler);
  }
}

function toCreateParams(label: string, o: WindowOptions): Record<string, unknown> {
  const p: Record<string, unknown> = { label, url: o.url ?? '' };
  const set = (key: string, value: unknown): void => {
    if (value !== undefined) {
      p[key] = value;
    }
  };
  set('title', o.title);
  set('width', o.width);
  set('height', o.height);
  set('min_width', o.minWidth);
  set('min_height', o.minHeight);
  set('x', o.x);
  set('y', o.y);
  set('center', o.center);
  set('frameless', o.frameless);
  set('headless_titlebar', o.headlessTitlebar);
  set('backdrop', o.backdrop);
  set('dark_mode', o.darkMode);
  set('always_on_top', o.alwaysOnTop);
  set('skip_taskbar', o.skipTaskbar);
  set('transparent', o.transparent);
  set('visible', o.visible);
  set('focus', o.focus);
  set('parent', o.parent);
  set('modal', o.modal);
  set('persist_state', o.persistState);
  set('hide_on_close', o.hideOnClose);
  return p;
}

export const windows = {
  /** Creates a window (needs a capability whose "windows" list matches the label). */
  async create(label: string, options: WindowOptions = {}): Promise<AppWindow> {
    const info = await call<WindowInfo>('Window.create', toCreateParams(label, options));
    return new AppWindow(info.label);
  },
  /** The window with this label, or null. */
  async get(label: string): Promise<AppWindow | null> {
    const all = await windows.list();
    return all.some((w) => w.label === label) ? new AppWindow(label) : null;
  },
  async getAll(): Promise<AppWindow[]> {
    return (await windows.list()).map((w) => new AppWindow(w.label));
  },
  async list(): Promise<WindowInfo[]> {
    return (await call<{ windows: WindowInfo[] }>('Window.list', {})).windows;
  },
  async focused(): Promise<AppWindow | null> {
    const hit = (await windows.list()).find((w) => w.focused);
    return hit ? new AppWindow(hit.label) : null;
  },
  current: (): AppWindow => AppWindow.current(),
  currentLabel,
  /** Event to one window by label ('*' = every window). */
  async emitTo<T>(label: string, event: string, payload?: T): Promise<void> {
    await call('Window.emit', { target: label, event, payload: payload === undefined ? null : payload });
  },
  /** Lifecycle events of every window. */
  on(event: WindowEventName, listener: (e: WindowLabelEvent) => void): Unsubscribe {
    return boson.on<WindowLabelEvent>(`window:${event}`, listener);
  },
};

export { BosonIpcError };
