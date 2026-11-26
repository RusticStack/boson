import type { BosonEvents } from './generated/boson-bindings';

export interface RpcErrorPayload {
  code: number;
  message: string;
}

export interface RpcResponse<T = unknown> {
  id: number;
  status: 'success' | 'error';
  data?: T;
  error?: RpcErrorPayload;
}

/** Backend-to-frontend event pushed by Bridge::emit / emit_to. RPC responses never carry `type`. */
export interface BosonEventEnvelope<T = unknown> {
  type: 'event';
  event: string;
  data: T;
}

export class BosonIpcError extends Error {
  public readonly code: number;

  constructor(message: string, code = -32000) {
    super(message);
    this.name = 'BosonIpcError';
    this.code = code;
  }
}

interface WebView2Host {
  postMessage(message: unknown): void;
  /** Sends DOM objects (File, FileSystemHandle) along with the message (WebView2 1.0.1370+). */
  postMessageWithAdditionalObjects?(message: unknown, additionalObjects: ArrayLike<unknown>): void;
  /** Host objects added with AddHostObjectToScript; `sync` holds the synchronous proxies. */
  hostObjects?: { sync?: Record<string, unknown> } & Record<string, unknown>;
  addEventListener(type: 'message', listener: (event: MessageEvent<unknown>) => void): void;
  removeEventListener(type: 'message', listener: (event: MessageEvent<unknown>) => void): void;
}

declare global {
  interface Window {
    chrome?: {
      webview?: WebView2Host;
    };
  }
}

interface PendingCall {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
}

/** The SyncBridge host object (C++ boson::ipc::SyncBridge, name "boson"). */
interface SyncHostObject {
  invoke(method: string, params?: string): string;
}

/** Global defined by C++ boson::ipc::BootValues before any page script runs (deep-frozen). */
export const BOOT_GLOBAL = '__BOSON_BOOT__';

type EventName = keyof BosonEvents & string;
type Listener = (data: unknown) => void;
export type Unsubscribe = () => void;

class BosonClient {
  private nextId = 1;
  private pending = new Map<number, PendingCall>();
  private listeners = new Map<string, Set<Listener>>();
  private initialized = false;
  private syncHost: SyncHostObject | null = null;

  private ensureListener(): WebView2Host {
    const webview = window.chrome?.webview;
    if (!webview) {
      throw new BosonIpcError(
        'WebView2 host bridge (window.chrome.webview) is unavailable in this environment.',
        -32001
      );
    }

    if (!this.initialized) {
      webview.addEventListener('message', (event: MessageEvent<unknown>) => {
        this.handleMessage(event.data);
      });
      this.initialized = true;
    }

    return webview;
  }

  private handleMessage(raw: unknown): void {
    if (Array.isArray(raw)) {
      // A batch of event envelopes (BridgeAttachOptions::batch_events): one postMessage, many events, in order.
      for (const item of raw) {
        this.handleMessage(item);
      }
      return;
    }
    let payload: (RpcResponse & Partial<BosonEventEnvelope>) | null = null;
    try {
      payload = (typeof raw === 'string' ? JSON.parse(raw) : raw) as RpcResponse & Partial<BosonEventEnvelope>;
    } catch {
      return;
    }
    if (!payload) {
      return;
    }

    if (payload.type === 'event' && typeof payload.event === 'string') {
      const set = this.listeners.get(payload.event);
      if (set) {
        if (set.size === 1) {
          // Common case: no snapshot copy of the listener set per event.
          const [only] = set;
          this.callListener(only, payload.event, payload.data);
        } else {
          for (const listener of [...set]) {
            this.callListener(listener, payload.event, payload.data);
          }
        }
      }
      return;
    }

    if (typeof payload.id !== 'number') {
      return;
    }

    const entry = this.pending.get(payload.id);
    if (!entry) {
      return;
    }
    this.pending.delete(payload.id);

    if (payload.status === 'success') {
      entry.resolve(payload.data);
    } else {
      const err = payload.error;
      entry.reject(new BosonIpcError(err?.message ?? 'Unknown C++ IPC error', err?.code ?? -32000));
    }
  }

  private callListener(listener: Listener, event: string, data: unknown): void {
    try {
      listener(data);
    } catch (err) {
      console.error(`[boson] listener for "${event}" threw`, err);
    }
  }

  public isAvailable(): boolean {
    return Boolean(window.chrome?.webview);
  }

  private syncHostObject(): SyncHostObject {
    if (this.syncHost) {
      return this.syncHost;
    }
    const host = window.chrome?.webview?.hostObjects?.sync?.boson as SyncHostObject | undefined;
    if (!host) {
      throw new BosonIpcError(
        'Synchronous IPC is unavailable: attach a boson::ipc::SyncBridge to this WebView (host object "boson").',
        -32001
      );
    }
    this.syncHost = host;
    return host;
  }

  /** true when a SyncBridge host object is attached to this WebView. */
  public isSyncAvailable(): boolean {
    try {
      this.syncHostObject();
      return true;
    } catch {
      return false;
    }
  }

  /**
   * Synchronous call through the SyncBridge host object (Electron's sendSync). Blocks this page's JS thread
   * until the C++ handler returns; the handler runs on the native UI thread, so only methods enabled for sync
   * (SyncController::boson_sync_methods / SyncBridge sync_methods) are reachable. Throws BosonIpcError.
   * Prefer boson.boot() for values known at startup and boson.call() for everything else.
   */
  public invokeSync<TResult = unknown, TParams = Record<string, never>>(method: string, params?: TParams): TResult {
    const raw = this.syncHostObject().invoke(method, params === undefined ? '{}' : JSON.stringify(params));
    let response: RpcResponse<TResult>;
    try {
      response = JSON.parse(raw) as RpcResponse<TResult>;
    } catch {
      throw new BosonIpcError('Invalid synchronous IPC response', -32603);
    }
    if (response.status === 'success') {
      return response.data as TResult;
    }
    throw new BosonIpcError(response.error?.message ?? 'Unknown C++ IPC error', response.error?.code ?? -32000);
  }

  /** The frozen boot-values object injected by C++ BootValues, or undefined when none was installed. */
  public bootValues(): Readonly<Record<string, unknown>> | undefined {
    return (window as unknown as Record<string, Readonly<Record<string, unknown>> | undefined>)[BOOT_GLOBAL];
  }

  /** One boot value (no IPC: a property read). undefined when absent. */
  public boot<T = unknown>(key: string): T | undefined {
    const values = this.bootValues();
    return values && Object.prototype.hasOwnProperty.call(values, key) ? (values[key] as T) : undefined;
  }

  public call<TResult = unknown, TParams = Record<string, never>>(
    method: string,
    params?: TParams
  ): Promise<TResult> {
    let webview: WebView2Host;
    try {
      webview = this.ensureListener();
    } catch (err) {
      return Promise.reject(err);
    }

    const id = this.nextId++;
    return new Promise<TResult>((resolve, reject) => {
      this.pending.set(id, {
        resolve: resolve as (value: unknown) => void,
        reject,
      });

      try {
        webview.postMessage({
          id,
          method,
          params: params ?? {},
        });
      } catch (err) {
        this.pending.delete(id);
        reject(err instanceof Error ? err : new Error(String(err)));
      }
    });
  }

  /** Subscribes to a backend event; returns the unsubscribe function. */
  public on<K extends EventName>(event: K, listener: (data: BosonEvents[K]) => void): Unsubscribe;
  public on<T = unknown>(event: string, listener: (data: T) => void): Unsubscribe;
  public on(event: string, listener: Listener): Unsubscribe {
    try {
      this.ensureListener();
    } catch {
      // Outside WebView2: keep the subscription so code paths stay identical; nothing will fire.
    }
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(listener);
    return () => {
      const current = this.listeners.get(event);
      current?.delete(listener);
      if (current && current.size === 0) {
        this.listeners.delete(event);
      }
    };
  }

  /** Like on(), but the listener runs at most once. */
  public once<K extends EventName>(event: K, listener: (data: BosonEvents[K]) => void): Unsubscribe;
  public once<T = unknown>(event: string, listener: (data: T) => void): Unsubscribe;
  public once(event: string, listener: Listener): Unsubscribe {
    const off = this.on(event, (data: unknown) => {
      off();
      listener(data);
    });
    return off;
  }

  /**
   * Marks `element` as the caption maximize button so hovering it shows Windows 11 Snap Layouts.
   * Its rect is re-sent whenever it moves or resizes. Returns a cleanup function.
   */
  public registerMaximizeButton(element: HTMLElement): Unsubscribe {
    if (!this.isAvailable()) {
      return () => {};
    }
    let frame = 0;
    const send = (): void => {
      frame = 0;
      const r = element.getBoundingClientRect();
      void this.call('Boson.set_maximize_button_rect', {
        x: r.left,
        y: r.top,
        width: r.width,
        height: r.height,
        visible: r.width > 0 && r.height > 0,
      }).catch(() => {});
    };
    const schedule = (): void => {
      if (frame === 0) {
        frame = requestAnimationFrame(send);
      }
    };
    const observer = new ResizeObserver(schedule);
    observer.observe(element);
    window.addEventListener('resize', schedule);
    schedule();
    return () => {
      observer.disconnect();
      window.removeEventListener('resize', schedule);
      if (frame !== 0) {
        cancelAnimationFrame(frame);
      }
      void this.call('Boson.set_maximize_button_rect', { x: 0, y: 0, width: 0, height: 0, visible: false }).catch(() => {});
    };
  }
}

export const boson = new BosonClient();
