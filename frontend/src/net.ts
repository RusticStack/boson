import { boson, type Unsubscribe } from './boson';

/** Request payloads are plain JSON objects matching the controller's request struct. */
type P = Record<string, unknown>;

/** Mirrors boson::net::HttpHeader. */
export interface HttpHeader {
  name: string;
  value: string;
}

/** Mirrors boson::ipc::HttpFetchResponse. */
export interface HttpFetchResponse {
  status: number;
  ok: boolean;
  headers: HttpHeader[];
  body: string;
  base64: boolean;
  final_url: string;
  protocol: string;
  redirects: number;
  attempts: number;
  from_cache: boolean;
}

export interface FetchOptions {
  method?: 'GET' | 'HEAD' | 'POST' | 'PUT' | 'PATCH' | 'DELETE' | 'OPTIONS';
  headers?: Record<string, string> | HttpHeader[];
  body?: string;
  timeoutMs?: number;
  /** Return the body base64 encoded (binary responses). */
  base64?: boolean;
  useCache?: boolean;
}

/** Payload of "http:progress". */
export interface HttpProgress {
  id: number;
  received: number;
  total?: number | null;
  fraction: number;
  bytes_per_second: number;
}

/** Payload of "http:done". */
export interface HttpDone {
  id: number;
  ok: boolean;
  cancelled: boolean;
  error: string;
  error_kind: string;
  status: number;
  path: string;
  bytes: number;
  digest: string;
}

export interface DownloadOptions {
  headers?: Record<string, string> | HttpHeader[];
  /** 'md5' | 'sha1' | 'sha256' | 'sha384' | 'sha512'; inferred from expectedHash when omitted. */
  hash?: string;
  expectedHash?: string;
  overwrite?: boolean;
  timeoutMs?: number;
  onProgress?: (progress: HttpProgress) => void;
  /** Aborting cancels the native download. */
  signal?: AbortSignal;
}

function toHeaders(headers: FetchOptions['headers']): HttpHeader[] {
  if (!headers) return [];
  if (Array.isArray(headers)) return headers;
  return Object.entries(headers).map(([name, value]) => ({ name, value }));
}

/** Case-insensitive header lookup on a fetch response. */
export function header(response: HttpFetchResponse, name: string): string | undefined {
  const lower = name.toLowerCase();
  return response.headers.find((h) => h.name.toLowerCase() === lower)?.value;
}

/** Typed wrappers over the Http controller (boson::ipc::HttpController). Hosts must be on its allow-list. */
export const http = {
  /** Native HTTP request (no CORS; WinHTTP with retries, redirects, gzip and the system proxy). */
  fetch(url: string, options: FetchOptions = {}): Promise<HttpFetchResponse> {
    return boson.call<HttpFetchResponse, P>('Http.fetch', {
      method: options.method ?? 'GET',
      url,
      headers: toHeaders(options.headers),
      body: options.body ?? '',
      timeout_ms: options.timeoutMs ?? null,
      base64: options.base64 ?? false,
      use_cache: options.useCache ?? true,
    });
  },

  /** GET and parse the body as JSON; rejects on a non-2xx status. */
  async json<T = unknown>(url: string, options: FetchOptions = {}): Promise<T> {
    const response = await http.fetch(url, options);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status} for ${url}`);
    }
    return JSON.parse(response.body) as T;
  },

  /** Downloads to a ScopedFs path; resolves with the "http:done" payload once verified (rejects on failure). */
  download(url: string, path: string, options: DownloadOptions = {}): Promise<HttpDone> {
    return new Promise<HttpDone>((resolve, reject) => {
      let id = -1;
      const pending: HttpDone[] = [];
      const offs: Unsubscribe[] = [];
      const finish = (done: HttpDone): void => {
        for (const off of offs) off();
        if (done.ok) resolve(done);
        else reject(Object.assign(new Error(done.cancelled ? 'Download cancelled' : done.error), { detail: done }));
      };
      if (options.onProgress) {
        const onProgress = options.onProgress;
        offs.push(boson.on<HttpProgress>('http:progress', (p) => {
          if (p.id === id) onProgress(p);
        }));
      }
      offs.push(boson.on<HttpDone>('http:done', (done) => {
        if (id < 0) pending.push(done);
        else if (done.id === id) finish(done);
      }));
      const onAbort = (): void => {
        if (id >= 0) void http.cancel(id);
      };
      options.signal?.addEventListener('abort', onAbort, { once: true });
      offs.push(() => options.signal?.removeEventListener('abort', onAbort));
      boson
        .call<{ id: number }, P>('Http.download', {
          url,
          path,
          headers: toHeaders(options.headers),
          hash: options.hash ?? '',
          expected_hash: options.expectedHash ?? '',
          overwrite: options.overwrite ?? true,
          timeout_ms: options.timeoutMs ?? null,
        })
        .then((started) => {
          id = started.id;
          if (options.signal?.aborted) onAbort();
          const early = pending.find((d) => d.id === id);
          if (early) finish(early);
        })
        .catch((err: unknown) => {
          for (const off of offs) off();
          reject(err);
        });
    });
  },

  cancel(id: number): Promise<boolean> {
    return boson.call<{ value: boolean }, P>('Http.cancel', { id }).then((r) => r.value);
  },

  allowedHosts(): Promise<string[]> {
    return boson.call<{ hosts: string[] }, P>('Http.allowed_hosts').then((r) => r.hosts);
  },
};
