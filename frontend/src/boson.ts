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

type EventName = keyof BosonEvents & string;
type Listener = (data: unknown) => void;
export type Unsubscribe = () => void;

class BosonClient {
  private nextId = 1;
  private pending = new Map<number, PendingCall>();
  private listeners = new Map<string, Set<Listener>>();
  private initialized = false;

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
        for (const listener of [...set]) {
          try {
            listener(payload.data);
          } catch (err) {
            console.error(`[boson] listener for "${payload.event}" threw`, err);
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

  public isAvailable(): boolean {
    return Boolean(window.chrome?.webview);
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
