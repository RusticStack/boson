import { boson, BosonIpcError, type Unsubscribe } from './boson';

/** How a payload crossed the boundary: a mapped ICoreWebView2SharedBuffer, or base64 inside a JSON message. */
export type TransferMode = 'shared' | 'base64';

export interface BinaryMessage<TMeta = unknown> {
  channel: string;
  /**
   * For 'shared' messages this views the shared memory itself and is detached as soon as the listener returns
   * (or, when the listener returns a Promise, once it settles). Copy with `data.slice()` to keep it longer.
   */
  data: Uint8Array;
  meta: TMeta;
  mode: TransferMode;
}

export interface BinaryResponse<TMeta = unknown> {
  data: Uint8Array;
  meta: TMeta;
  mode: TransferMode;
  /** Returns a zero-copy buffer to the native pool. A no-op unless the request used `zeroCopy: true`. */
  release(): void;
}

export interface RequestOptions {
  /** Resolve with a view on the shared memory instead of a copy. You must call release() when done. */
  zeroCopy?: boolean;
}

export type UploadData = ArrayBuffer | ArrayBufferView | string;

/** window.__bosonTransfer, installed by BinaryTransport::attach (or BinaryTransport::page_script()). */
export interface BosonTransferRuntime {
  readonly version: number;
  onBinary(channel: string, listener: (message: BinaryMessage) => void | Promise<unknown>): Unsubscribe;
  upload(channel: string, data: UploadData, meta?: unknown): Promise<unknown>;
  request(channel: string, params?: unknown, options?: RequestOptions): Promise<BinaryResponse>;
}

/** Native Transfer.capabilities() (TransferController). */
export interface TransferCapabilities {
  mode: 'shared_buffer' | 'base64';
  shared_buffer: boolean;
  max_upload_bytes: number;
  stream_base_url: string;
}

interface DropHost {
  postMessageWithAdditionalObjects?: (message: unknown, objects: ArrayLike<unknown>) => void;
}

declare global {
  interface Window {
    __bosonTransfer?: BosonTransferRuntime;
  }
}

function currentRuntime(): BosonTransferRuntime | undefined {
  return window.__bosonTransfer;
}

/**
 * Resolves with the page runtime. BinaryTransport::attach may inject it after the page's own scripts ran, so
 * this waits up to timeoutMs for it to appear.
 */
export function whenTransferReady(timeoutMs = 5000): Promise<BosonTransferRuntime> {
  const ready = currentRuntime();
  if (ready) {
    return Promise.resolve(ready);
  }
  if (!boson.isAvailable()) {
    return Promise.reject(new BosonIpcError('WebView2 host bridge (window.chrome.webview) is unavailable.', -32001));
  }
  return new Promise((resolve, reject) => {
    const started = performance.now();
    const poll = (): void => {
      const rt = currentRuntime();
      if (rt) {
        resolve(rt);
      } else if (performance.now() - started > timeoutMs) {
        reject(new BosonIpcError('Boson transfer runtime not installed (attach a BinaryTransport to this WebView).', -32002));
      } else {
        setTimeout(poll, 16);
      }
    };
    poll();
  });
}

/** Subscribes to binary payloads the backend sends with BinaryTransport::send_binary(webview, channel, ...). */
export function onBinary<TMeta = unknown>(
  channel: string,
  listener: (message: BinaryMessage<TMeta>) => void | Promise<unknown>
): Unsubscribe {
  let off: Unsubscribe | null = null;
  let cancelled = false;
  const typed = listener as (message: BinaryMessage) => void | Promise<unknown>;
  const rt = currentRuntime();
  if (rt) {
    off = rt.onBinary(channel, typed);
  } else {
    whenTransferReady()
      .then((runtime) => {
        if (!cancelled) {
          off = runtime.onBinary(channel, typed);
        }
      })
      .catch((err: unknown) => console.error('[boson] onBinary:', err));
  }
  return () => {
    cancelled = true;
    off?.();
  };
}

/** Asks a BinaryTransport::on_request(channel) handler for binary data. */
export async function requestBinary<TMeta = unknown>(
  channel: string,
  params?: unknown,
  options?: RequestOptions
): Promise<BinaryResponse<TMeta>> {
  const rt = await whenTransferReady();
  return (await rt.request(channel, params ?? null, options)) as BinaryResponse<TMeta>;
}

/**
 * Sends a large binary payload to a BinaryTransport::on_upload(channel) handler: written once into a native
 * shared buffer when available, base64 otherwise. Resolves with the JSON value the handler returned.
 */
export async function upload<TResult = unknown>(channel: string, data: UploadData, meta?: unknown): Promise<TResult> {
  const rt = await whenTransferReady();
  return (await rt.upload(channel, data, meta ?? null)) as TResult;
}

/** Requires TransferController in the Router. */
export function transferCapabilities(): Promise<TransferCapabilities> {
  return boson.call<TransferCapabilities>('Transfer.capabilities');
}

export interface DropZoneOptions<TMeta = unknown> {
  /** Reported to C++ as FileDropEvent::zone. */
  zone: string;
  /** Reported to C++ as FileDropEvent::metadata_json. */
  meta?: TMeta;
  /** CSS class toggled while files hover over the element. Default 'boson-drop-hover'. */
  hoverClass?: string;
  /** Called in the page with the dropped File objects (names, sizes) after they were posted to C++. */
  onDrop?: (files: File[], event: DragEvent) => void;
}

function hasFiles(event: DragEvent): boolean {
  const types = event.dataTransfer?.types;
  return types !== undefined && Array.prototype.indexOf.call(types, 'Files') >= 0;
}

/**
 * Turns element into a file drop zone. Drops are posted with chrome.webview.postMessageWithAdditionalObjects, so
 * C++ (FileDrop) receives the absolute paths. Returns a cleanup function.
 */
export function createDropZone<TMeta = unknown>(element: HTMLElement, options: DropZoneOptions<TMeta>): Unsubscribe {
  const host = window.chrome?.webview as unknown as DropHost | undefined;
  const hoverClass = options.hoverClass ?? 'boson-drop-hover';
  let depth = 0;

  const onEnter = (e: DragEvent): void => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth += 1;
    element.classList.add(hoverClass);
  };
  const onOver = (e: DragEvent): void => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
  };
  const onLeave = (e: DragEvent): void => {
    if (!hasFiles(e)) return;
    depth = Math.max(0, depth - 1);
    if (depth === 0) element.classList.remove(hoverClass);
  };
  const onDrop = (e: DragEvent): void => {
    if (!hasFiles(e)) return;
    e.preventDefault(); // also tells FileDrop's page-wide handler that this drop is handled
    depth = 0;
    element.classList.remove(hoverClass);
    const files = Array.from(e.dataTransfer?.files ?? []);
    if (files.length === 0) return;
    if (host?.postMessageWithAdditionalObjects) {
      host.postMessageWithAdditionalObjects(
        { __boson_transfer: 'drop', zone: options.zone, x: e.clientX, y: e.clientY, meta: options.meta ?? null },
        files
      );
    }
    options.onDrop?.(files, e);
  };

  element.addEventListener('dragenter', onEnter);
  element.addEventListener('dragover', onOver);
  element.addEventListener('dragleave', onLeave);
  element.addEventListener('drop', onDrop);
  return () => {
    element.removeEventListener('dragenter', onEnter);
    element.removeEventListener('dragover', onOver);
    element.removeEventListener('dragleave', onLeave);
    element.removeEventListener('drop', onDrop);
    element.classList.remove(hoverClass);
  };
}

/** URL of a StreamServer resource, e.g. mediaUrl('/videos', 'clips/intro.mp4') -> boson://media/videos/clips/intro.mp4. */
export function mediaUrl(mount: string, relativePath: string, host = 'media'): string {
  const prefix = mount.startsWith('/') ? mount : `/${mount}`;
  const encoded = relativePath
    .split('/')
    .filter((s) => s.length > 0)
    .map((s) => encodeURIComponent(s))
    .join('/');
  return `boson://${host}${prefix.replace(/\/+$/, '')}/${encoded}`;
}
