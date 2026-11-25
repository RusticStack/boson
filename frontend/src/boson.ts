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

class BosonClient {
  private nextId = 1;
  private pending = new Map<number, PendingCall>();
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
    let payload: RpcResponse | null = null;
    try {
      payload = (typeof raw === 'string' ? JSON.parse(raw) : raw) as RpcResponse;
    } catch {
      return;
    }

    if (!payload || typeof payload.id !== 'number') {
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
}

export const boson = new BosonClient();
