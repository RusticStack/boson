/**
 * <boson-webview>: Boson's replacement for Electron's <webview> tag (built-in browser tabs, previews,
 * Playwright-driven pages). Each element owns a native guest WebView (boson::webview::Guest, controller
 * "Guest") that is kept on top of the element's box: position, size, overflow clipping, visibility and
 * stacking are measured here once per animation frame for every element together and applied natively in one
 * call (Guest.set_layout). Nothing runs while nothing moves: observers mark elements dirty and a single
 * requestAnimationFrame flushes them; CSS transitions / animations keep the loop alive only while they run.
 *
 *   <boson-webview src="https://example.com" partition="persist:docs" allowpopups></boson-webview>
 *
 * Attributes: src, partition ("persist:x" | "x" in-memory | "inprivate"), preload (URL resolved by the app),
 * allowpopups, popups ("deny" | "guest" | "window"), useragent, zoom, permissions / basicauth /
 * certificateerrors ("default" | "deny" | "allow" | "ask"), reservedaccelerators ("Ctrl+W,Ctrl+T"),
 * console (report console-message), z (stacking among guests), guestinstance (adopt an existing guest, e.g.
 * a popup from "new-window").
 *
 * Inside the guest, window.bosonGuest.sendToHost(channel, ...args) raises "ipc-message" on the element and
 * element.send(channel, ...args) reaches bosonGuest.on(channel, (event, ...args) => ...).
 */
import { boson, type Unsubscribe } from './boson';
import type {
  GuestAccelerator,
  GuestCaptureResult,
  GuestCdpEvent,
  GuestCreateRequest,
  GuestFocusOut,
  GuestFocus,
  GuestInfo,
  GuestLayoutItem,
  GuestPdfResult,
  PrintOptions,
  PrintResult,
} from './generated/boson-bindings';

export const WEBVIEW_TAG = 'boson-webview';

/** guest:* bridge event -> DOM event name dispatched on the element (Electron names where they exist). */
const EVENT_MAP: Record<string, string> = {
  'guest:did-start-loading': 'did-start-loading',
  'guest:did-navigate': 'did-navigate',
  'guest:did-navigate-in-page': 'did-navigate-in-page',
  'guest:dom-ready': 'dom-ready',
  'guest:did-finish-load': 'did-finish-load',
  'guest:did-fail-load': 'did-fail-load',
  'guest:page-title-updated': 'page-title-updated',
  'guest:page-favicon-updated': 'page-favicon-updated',
  'guest:new-window': 'new-window',
  'guest:console-message': 'console-message',
  'guest:ipc-message': 'ipc-message',
  'guest:crashed': 'crashed',
  'guest:close': 'close',
  'guest:audio-state': 'audio-state',
  'guest:download-started': 'download-started',
  'guest:download-updated': 'download-updated',
  'guest:download-done': 'download-done',
  'guest:permission-request': 'permission-request',
  'guest:login': 'login',
  'guest:certificate-error': 'certificate-error',
  'guest:select-client-certificate': 'select-client-certificate',
  'guest:found-in-page': 'found-in-page',
  'guest:cdp-event': 'cdp-event',
  'guest:destroyed': 'destroyed',
};

/** Event dispatched on <boson-webview>: Electron-style fields are copied onto the event; detail has them too. */
export class BosonWebViewEvent<T extends object = Record<string, unknown>> extends Event {
  public readonly detail: T;
  [key: string]: unknown;

  constructor(type: string, detail: T) {
    super(type, { bubbles: false, cancelable: false });
    this.detail = detail;
    for (const [k, v] of Object.entries(detail)) {
      if (!(k in this)) {
        (this as Record<string, unknown>)[k] = v;
      }
    }
  }
}

export interface CapturedImage {
  mimeType: string;
  /** base64 payload */
  data: string;
  size: number;
  toDataURL(): string;
  toBlob(): Blob;
}

export interface FindInPageOptions {
  forward?: boolean;
  findNext?: boolean;
  matchCase?: boolean;
}

function base64ToBytes(data: string): Uint8Array<ArrayBuffer> {
  const binary = atob(data);
  const bytes = new Uint8Array(new ArrayBuffer(binary.length));
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

// ---- Global routing ------------------------------------------------------------------------------------------

const elements = new Map<number, BosonWebViewElement>();
let routingInstalled = false;
let cleanedUp: Promise<unknown> | null = null;

/** Called once per document: guests left over from the previous document of this page are destroyed. */
function cleanupPreviousDocument(): Promise<unknown> {
  if (!cleanedUp) {
    cleanedUp = boson.call('Guest.destroy_all').catch(() => undefined);
  }
  return cleanedUp;
}

function installRouting(): void {
  if (routingInstalled) {
    return;
  }
  routingInstalled = true;
  for (const [bridgeEvent, domEvent] of Object.entries(EVENT_MAP)) {
    boson.on<{ id: number }>(bridgeEvent, (data) => {
      const el = elements.get(data.id);
      if (el) {
        el._dispatchGuestEvent(domEvent, data as unknown as Record<string, unknown>);
      }
    });
  }
  boson.on<GuestFocusOut>('guest:focus-out', (data) => elements.get(data.id)?._focusOut(data.direction));
  boson.on<GuestFocus>('guest:focus', (data) => elements.get(data.id)?._guestFocus(data.focused));
  boson.on<GuestAccelerator>('guest:accelerator', (data) => elements.get(data.id)?._accelerator(data));
}

// ---- Layout scheduler ----------------------------------------------------------------------------------------

interface SentPlacement {
  key: string;
}

const EPSILON = 0.01;

function sameRect(a: DOMRectReadOnly, x: number, y: number, w: number, h: number): boolean {
  return Math.abs(a.left - x) < EPSILON && Math.abs(a.top - y) < EPSILON && Math.abs(a.width - w) < EPSILON && Math.abs(a.height - h) < EPSILON;
}

function containsToken(value: string, token: string): boolean {
  return value.split(' ').includes(token);
}

/** Whether node is the containing block of descendants with the given position (CSS 2 / Transforms / Containment). */
function isContainingBlockFor(node: Element, style: CSSStyleDeclaration, position: string): boolean {
  const forFixed =
    style.transform !== 'none' ||
    style.perspective !== 'none' ||
    style.filter !== 'none' ||
    containsToken(style.contain, 'paint') ||
    containsToken(style.contain, 'layout') ||
    containsToken(style.contain, 'strict') ||
    containsToken(style.contain, 'content') ||
    containsToken(style.willChange, 'transform');
  if (position === 'fixed') {
    return forFixed;
  }
  if (position === 'absolute') {
    return forFixed || style.position !== 'static';
  }
  return node !== null; // static / relative / sticky: the parent box
}

/**
 * Visible part of el after the viewport and every overflow-clipping ancestor on its containing-block chain
 * (absolutely / fixed positioned boxes escape the overflow of ancestors that do not contain them), or null when
 * fully visible, or 'hidden' when nothing is visible.
 */
function clipRectOf(el: Element, rect: DOMRectReadOnly): { x: number; y: number; width: number; height: number } | null | 'hidden' {
  let left = 0;
  let top = 0;
  let right = document.documentElement.clientWidth || window.innerWidth;
  let bottom = document.documentElement.clientHeight || window.innerHeight;
  const rootOverflowVisible = getComputedStyle(document.documentElement).overflow === 'visible';
  let position = getComputedStyle(el).position;
  for (let node = el.parentElement; node && node !== document.documentElement; node = node.parentElement) {
    const style = getComputedStyle(node);
    if (!isContainingBlockFor(node, style, position)) {
      continue;
    }
    position = style.position;
    const clips =
      (style.overflowX !== 'visible' || style.overflowY !== 'visible' || containsToken(style.contain, 'paint')) &&
      !(node === document.body && rootOverflowVisible); // body overflow then applies to the viewport
    if (!clips) {
      continue;
    }
    const r = node.getBoundingClientRect();
    const cl = r.left + node.clientLeft;
    const ct = r.top + node.clientTop;
    left = Math.max(left, cl);
    top = Math.max(top, ct);
    right = Math.min(right, cl + node.clientWidth);
    bottom = Math.min(bottom, ct + node.clientHeight);
  }
  left = Math.max(left, rect.left);
  top = Math.max(top, rect.top);
  right = Math.min(right, rect.right);
  bottom = Math.min(bottom, rect.bottom);
  if (right <= left || bottom <= top) {
    return 'hidden';
  }
  if (sameRect(rect, left, top, right - left, bottom - top)) {
    return null;
  }
  return { x: left, y: top, width: right - left, height: bottom - top };
}

class LayoutScheduler {
  private readonly tracked = new Set<BosonWebViewElement>();
  private readonly dirty = new Set<BosonWebViewElement>();
  private readonly sent = new WeakMap<BosonWebViewElement, SentPlacement>();
  private frame = 0;
  private fallback = 0;
  private animating = 0;
  private installed = false;
  private resizeObserver: ResizeObserver | null = null;
  private intersectionObserver: IntersectionObserver | null = null;
  private mutationObserver: MutationObserver | null = null;
  private dprQuery: MediaQueryList | null = null;
  /** Frames flushed and items sent, for diagnostics / benchmarks. */
  public stats = { frames: 0, items: 0, calls: 0, lastFlushMs: 0 };

  public track(el: BosonWebViewElement): void {
    this.install();
    this.tracked.add(el);
    this.resizeObserver?.observe(el);
    this.intersectionObserver?.observe(el);
    this.markDirty(el);
  }

  public untrack(el: BosonWebViewElement): void {
    this.tracked.delete(el);
    this.dirty.delete(el);
    this.sent.delete(el);
    this.resizeObserver?.unobserve(el);
    this.intersectionObserver?.unobserve(el);
  }

  public markDirty(el?: BosonWebViewElement): void {
    if (el) {
      this.dirty.add(el);
    } else {
      for (const t of this.tracked) {
        this.dirty.add(t);
      }
    }
    this.schedule();
  }

  /** Forces a re-send of el's placement on the next frame (after the guest was re-created / adopted). */
  public invalidate(el: BosonWebViewElement): void {
    this.sent.delete(el);
    this.markDirty(el);
  }

  private schedule(): void {
    if (this.frame === 0 && (this.dirty.size > 0 || this.animating > 0)) {
      // rAF batches with the page's own layout; the timer covers documents that render no frames (hidden or
      // occluded host windows), where guests must still be moved / hidden. Whichever fires first wins.
      this.frame = requestAnimationFrame(() => this.flush());
      this.fallback = window.setTimeout(() => this.flush(), 100);
    }
  }

  private install(): void {
    if (this.installed) {
      return;
    }
    this.installed = true;
    const all = (): void => this.markDirty();
    this.resizeObserver = new ResizeObserver((entries) => {
      for (const e of entries) {
        if (e.target instanceof BosonWebViewElement) {
          this.markDirty(e.target);
        } else {
          this.markDirty();
        }
      }
    });
    this.resizeObserver.observe(document.documentElement);
    // Only a change trigger: whether the box is on screen is computed in flush (viewport + overflow clip), as
    // isIntersecting is not updated for documents that render no frames.
    this.intersectionObserver = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (e.target instanceof BosonWebViewElement) {
          this.markDirty(e.target);
        }
      }
    });
    // Position-only moves (content inserted above, class / style toggles) do not resize the element.
    this.mutationObserver = new MutationObserver(all);
    this.mutationObserver.observe(document.documentElement, { subtree: true, childList: true, attributes: true, attributeFilter: ['style', 'class', 'hidden', 'open'] });
    window.addEventListener('scroll', all, { capture: true, passive: true });
    window.addEventListener('resize', all, { passive: true });
    document.addEventListener('visibilitychange', all);
    const start = (): void => {
      this.animating++;
      this.schedule();
    };
    const end = (): void => {
      this.animating = Math.max(0, this.animating - 1);
      this.markDirty();
    };
    document.addEventListener('transitionrun', start, true);
    document.addEventListener('transitionend', end, true);
    document.addEventListener('transitioncancel', end, true);
    document.addEventListener('animationstart', start, true);
    document.addEventListener('animationend', end, true);
    document.addEventListener('animationcancel', end, true);
    this.watchDpr();
  }

  private watchDpr(): void {
    this.dprQuery?.removeEventListener('change', this.onDpr);
    this.dprQuery = matchMedia('(resolution: ' + window.devicePixelRatio + 'dppx)');
    this.dprQuery.addEventListener('change', this.onDpr);
  }

  private readonly onDpr = (): void => {
    for (const t of this.tracked) {
      this.sent.delete(t);
    }
    this.watchDpr();
    this.markDirty();
  };

  private flush(): void {
    if (this.frame !== 0) {
      cancelAnimationFrame(this.frame);
    }
    clearTimeout(this.fallback);
    this.frame = 0;
    this.fallback = 0;
    const t0 = performance.now();
    const targets = this.animating > 0 ? [...this.tracked] : [...this.dirty];
    this.dirty.clear();
    const items: GuestLayoutItem[] = [];
    for (const el of targets) {
      const id = el.getWebContentsId();
      if (id === 0) {
        continue;
      }
      const rect = el.getBoundingClientRect();
      // Not document.visibilityState: guests are child windows of the host, so a minimized or hidden host
      // window hides them already, and an occluded one must keep them in place.
      let visible =
        el.isConnected &&
        rect.width > 0 &&
        rect.height > 0 &&
        (typeof el.checkVisibility !== 'function' || el.checkVisibility({ checkOpacity: false, checkVisibilityCSS: true }));
      let clip: { x: number; y: number; width: number; height: number } | null = null;
      if (visible) {
        const c = clipRectOf(el, rect);
        if (c === 'hidden') {
          visible = false;
        } else {
          clip = c;
        }
      }
      const z = Number(el.getAttribute('z') ?? 0) || 0;
      const item: GuestLayoutItem = {
        id,
        rect: { x: rect.left, y: rect.top, width: rect.width, height: rect.height },
        clip,
        visible,
        z,
      };
      const key = JSON.stringify(item);
      if (this.sent.get(el)?.key === key) {
        continue;
      }
      this.sent.set(el, { key });
      items.push(item);
    }
    if (items.length > 0) {
      this.stats.items += items.length;
      this.stats.calls++;
      void boson.call('Guest.set_layout', { items, device_pixel_ratio: window.devicePixelRatio }).catch(() => undefined);
    }
    this.stats.frames++;
    this.stats.lastFlushMs = performance.now() - t0;
    this.schedule();
  }
}

export const layoutScheduler = new LayoutScheduler();

// ---- Focus helpers ---------------------------------------------------------------------------------------------

const TABBABLE =
  'a[href],area[href],button:not([disabled]),input:not([disabled]):not([type="hidden"]),select:not([disabled]),' +
  'textarea:not([disabled]),iframe,[contenteditable=""],[contenteditable="true"],[tabindex],' + WEBVIEW_TAG;

function tabbables(): HTMLElement[] {
  return [...document.querySelectorAll<HTMLElement>(TABBABLE)].filter(
    (el) => el.tabIndex >= 0 && (typeof el.checkVisibility !== 'function' || el.checkVisibility()),
  );
}

let lastTabShift: boolean | null = null;
document.addEventListener(
  'keydown',
  (e) => {
    if (e.key === 'Tab') {
      lastTabShift = e.shiftKey;
      setTimeout(() => (lastTabShift = null), 0);
    }
  },
  true,
);

// ---- The element ------------------------------------------------------------------------------------------------

export class BosonWebViewElement extends HTMLElement {
  public static readonly observedAttributes = [
    'src', 'zoom', 'useragent', 'allowpopups', 'popups', 'permissions', 'basicauth', 'certificateerrors', 'console', 'z',
  ];

  /** Layout scheduler counters (frames flushed, items and Guest.set_layout calls sent, last flush cost). */
  public static layoutStats(): { frames: number; items: number; calls: number; lastFlushMs: number } {
    return { ...layoutScheduler.stats };
  }

  private id_ = 0;
  private suppressSrc = false;
  private creating: Promise<number> | null = null;
  private info: Partial<GuestInfo> = {};
  private disposeTimer = 0;
  private suppressFocusForward = false;
  private consoleWanted = false;
  private readonly cdpListeners = new Map<string, Set<(params: unknown, sessionId: string) => void>>();

  constructor() {
    super();
    // No attributes here: document.createElement() rejects custom elements whose constructor adds any.
    this.addEventListener('focus', () => this.onHostFocus());
  }

  // ---- lifecycle ----
  public connectedCallback(): void {
    if (!this.hasAttribute('tabindex')) {
      this.tabIndex = 0; // focusable, so Tab order reaches the guest
    }
    if (this.disposeTimer !== 0) {
      clearTimeout(this.disposeTimer);
      this.disposeTimer = 0;
    }
    if (!this.style.display && getComputedStyle(this).display === 'inline') {
      this.style.display = 'inline-block';
    }
    void this.ensureGuest();
  }

  public disconnectedCallback(): void {
    // Moving the element (removal + insertion in the same task) keeps the guest alive.
    this.disposeTimer = window.setTimeout(() => {
      this.disposeTimer = 0;
      if (!this.isConnected) {
        void this.dispose();
      }
    }, 0);
    layoutScheduler.markDirty(this);
  }

  public attributeChangedCallback(name: string, oldValue: string | null, value: string | null): void {
    if (this.id_ === 0 || oldValue === value) {
      return;
    }
    switch (name) {
      case 'src':
        if (!this.suppressSrc && value && value !== this.info.url) {
          void this.loadURL(value);
        }
        break;
      case 'zoom':
        void this.setZoomFactor(Number(value ?? 1) || 1);
        break;
      case 'useragent':
        void this.setUserAgent(value ?? '');
        break;
      case 'allowpopups':
      case 'popups':
        void this.call('Guest.set_options', { id: this.id_, popups: this.popupPolicy() });
        break;
      case 'permissions':
      case 'basicauth':
      case 'certificateerrors':
        void this.call('Guest.set_options', {
          id: this.id_,
          permissions: this.getAttribute('permissions') ?? 'default',
          basic_auth: this.getAttribute('basicauth') ?? 'default',
          certificate_errors: this.getAttribute('certificateerrors') ?? 'deny',
        });
        break;
      case 'console':
        this.enableConsole(value !== null);
        break;
      case 'z':
        layoutScheduler.markDirty(this);
        break;
    }
  }

  /** Resolves with the guest id once the native guest exists. */
  public whenReady(): Promise<number> {
    return this.id_ !== 0 ? Promise.resolve(this.id_) : this.ensureGuest();
  }

  private popupPolicy(): string {
    return this.getAttribute('popups') ?? (this.hasAttribute('allowpopups') ? 'guest' : 'deny');
  }

  private ensureGuest(): Promise<number> {
    if (this.id_ !== 0) {
      layoutScheduler.track(this);
      return Promise.resolve(this.id_);
    }
    if (this.creating) {
      return this.creating;
    }
    installRouting();
    const adopt = Number(this.getAttribute('guestinstance') ?? 0);
    const request: GuestCreateRequest = {
      src: this.getAttribute('src') ?? '',
      partition: this.getAttribute('partition') ?? '',
      preload: this.getAttribute('preload') ?? '',
      preload_script: '',
      allowpopups: this.hasAttribute('allowpopups'),
      popups: this.popupPolicy(),
      useragent: this.getAttribute('useragent') ?? '',
      zoom: Number(this.getAttribute('zoom') ?? 1) || 1,
      permissions: this.getAttribute('permissions') ?? 'default',
      basic_auth: this.getAttribute('basicauth') ?? 'default',
      certificate_errors: this.getAttribute('certificateerrors') ?? 'deny',
      client_certificates: 'default',
      devtools: !this.hasAttribute('nodevtools'),
      context_menus: !this.hasAttribute('nocontextmenu'),
      console_messages: this.hasAttribute('console') || this.consoleWanted,
      forward_accelerators: !this.hasAttribute('noaccelerators'),
      hide_download_ui: !this.hasAttribute('downloadui'),
      reserved_accelerators: (this.getAttribute('reservedaccelerators') ?? '')
        .split(',')
        .map((s) => s.trim())
        .filter((s) => s.length > 0),
    };
    this.creating = (async () => {
      if (adopt === 0) {
        await cleanupPreviousDocument();
      }
      const info = adopt !== 0
        ? await boson.call<GuestInfo, { id: number }>('Guest.adopt', { id: adopt })
        : await boson.call<GuestInfo, GuestCreateRequest>('Guest.create', request);
      this.id_ = info.id;
      this.info = info;
      elements.set(info.id, this);
      this.creating = null;
      if (this.isConnected) {
        layoutScheduler.track(this);
      }
      if (adopt !== 0 && (this.hasAttribute('console') || this.consoleWanted)) {
        this.enableConsole(true);
      }
      this.dispatchEvent(new BosonWebViewEvent('guest-ready', { id: info.id }));
      return info.id;
    })();
    this.creating.catch((err: unknown) => {
      this.creating = null;
      this.dispatchEvent(new BosonWebViewEvent('did-fail-load', { errorCode: -1, errorDescription: String(err), url: request.src }));
    });
    return this.creating;
  }

  private async dispose(): Promise<void> {
    const id = this.id_;
    layoutScheduler.untrack(this);
    if (id === 0) {
      return;
    }
    this.id_ = 0;
    elements.delete(id);
    await boson.call('Guest.destroy', { id }).catch(() => undefined);
  }

  private async call<R = unknown>(method: string, params: Record<string, unknown> = {}): Promise<R> {
    const id = await this.whenReady();
    return boson.call<R, Record<string, unknown>>(method, { id, ...params });
  }

  /** Intercepts console-message listeners so the CDP Runtime domain is only enabled when someone listens. */
  public override addEventListener(type: string, listener: EventListenerOrEventListenerObject, options?: boolean | AddEventListenerOptions): void {
    if (type === 'console-message') {
      this.enableConsole(true);
    }
    super.addEventListener(type, listener, options);
  }

  private enableConsole(on: boolean): void {
    this.consoleWanted = on;
    if (this.id_ !== 0) {
      void boson.call('Guest.set_options', { id: this.id_, console_messages: on }).catch(() => undefined);
    }
  }

  // ---- routed native events (internal) ----
  public _dispatchGuestEvent(type: string, data: Record<string, unknown>): void {
    switch (type) {
      case 'did-navigate':
        this.info.url = String(data.url ?? '');
        this.info.can_go_back = Boolean(data.can_go_back);
        this.info.can_go_forward = Boolean(data.can_go_forward);
        this.info.loading = false;
        break;
      case 'did-navigate-in-page':
        this.info.url = String(data.url ?? '');
        break;
      case 'did-start-loading':
        this.info.loading = true;
        break;
      case 'did-finish-load':
      case 'did-fail-load':
        this.info.loading = false;
        break;
      case 'page-title-updated':
        this.info.title = String(data.title ?? '');
        break;
      case 'audio-state':
        this.info.audible = Boolean(data.audible);
        this.info.muted = Boolean(data.muted);
        this.dispatchEvent(new BosonWebViewEvent(data.audible ? 'media-started-playing' : 'media-paused', data));
        break;
      case 'crashed':
        this.info.crashed = true;
        this.dispatchEvent(new BosonWebViewEvent('render-process-gone', { ...data, details: { reason: data.kind, exitCode: data.exit_code } }));
        break;
      case 'cdp-event': {
        const e = data as unknown as GuestCdpEvent;
        for (const fn of this.cdpListeners.get(e.method) ?? []) {
          fn(e.params, e.session_id);
        }
        break;
      }
      case 'destroyed':
        if (this.id_ === Number(data.id)) {
          layoutScheduler.untrack(this);
          elements.delete(this.id_);
          this.id_ = 0;
        }
        break;
    }
    const detail = { ...data } as Record<string, unknown>;
    if (type === 'did-fail-load') {
      detail.errorCode = data.error_code;
      detail.errorDescription = data.error_description;
      detail.validatedURL = data.url;
      detail.isMainFrame = true;
    } else if (type === 'did-navigate') {
      detail.httpResponseCode = data.http_response_code;
    } else if (type === 'new-window') {
      detail.guestInstanceId = data.new_guest_id;
    } else if (type === 'found-in-page') {
      detail.result = {
        requestId: data.request_id,
        matches: data.matches,
        activeMatchOrdinal: data.active_match_ordinal,
        finalUpdate: data.final_update,
      };
    } else if (type === 'console-message') {
      detail.sourceId = data.source_id;
    }
    this.dispatchEvent(new BosonWebViewEvent(type, detail));
  }

  public _focusOut(direction: string): void {
    const list = tabbables();
    const index = list.indexOf(this);
    const next = direction === 'previous' ? list[index - 1] : list[index + 1];
    this.dispatchEvent(new BosonWebViewEvent('focus-out', { direction }));
    if (next) {
      next.focus();
    } else {
      this.blur();
    }
  }

  public _guestFocus(focused: boolean): void {
    if (focused && document.activeElement !== this) {
      this.suppressFocusForward = true;
      this.focus({ preventScroll: true });
      this.suppressFocusForward = false;
    }
    this.dispatchEvent(new BosonWebViewEvent(focused ? 'guest-focus' : 'guest-blur', { focused }));
  }

  public _accelerator(a: GuestAccelerator): void {
    const init: KeyboardEventInit & { keyCode: number; which: number } = {
      key: a.key,
      code: a.code,
      ctrlKey: a.ctrl,
      altKey: a.alt,
      shiftKey: a.shift,
      metaKey: a.meta,
      repeat: a.repeat,
      bubbles: true,
      cancelable: true,
      composed: true,
      keyCode: a.key_code,
      which: a.key_code,
    };
    this.dispatchEvent(new KeyboardEvent('keydown', init));
  }

  private onHostFocus(): void {
    if (this.suppressFocusForward || this.id_ === 0) {
      return;
    }
    const direction = lastTabShift === null ? 'programmatic' : lastTabShift ? 'previous' : 'next';
    void boson.call('Guest.focus', { id: this.id_, direction }).catch(() => undefined);
  }

  // ---- Electron <webview> API ----
  public getWebContentsId(): number {
    return this.id_;
  }
  public getURL(): string {
    return this.info.url ?? this.getAttribute('src') ?? '';
  }
  public getTitle(): string {
    return this.info.title ?? '';
  }
  public isLoading(): boolean {
    return Boolean(this.info.loading);
  }
  public isCrashed(): boolean {
    return Boolean(this.info.crashed);
  }
  public canGoBack(): boolean {
    return Boolean(this.info.can_go_back);
  }
  public canGoForward(): boolean {
    return Boolean(this.info.can_go_forward);
  }
  public isAudioMuted(): boolean {
    return Boolean(this.info.muted);
  }
  public isCurrentlyAudible(): boolean {
    return Boolean(this.info.audible);
  }
  public getZoomFactor(): number {
    return this.info.zoom ?? 1;
  }

  public get src(): string {
    return this.getAttribute('src') ?? '';
  }
  public set src(value: string) {
    this.setAttribute('src', value);
  }

  public async loadURL(url: string): Promise<void> {
    this.suppressSrc = true;
    try {
      this.setAttribute('src', url);
    } finally {
      this.suppressSrc = false;
    }
    this.info.url = url;
    await this.call('Guest.load_url', { url });
  }
  public async goBack(): Promise<void> {
    await this.call('Guest.go_back');
  }
  public async goForward(): Promise<void> {
    await this.call('Guest.go_forward');
  }
  public async reload(): Promise<void> {
    await this.call('Guest.reload');
  }
  public async stop(): Promise<void> {
    await this.call('Guest.stop');
  }
  /** Runs code in the guest's top document (awaiting a returned Promise); rejects on exceptions. */
  public async executeJavaScript<T = unknown>(code: string, userGesture = false): Promise<T> {
    const r = await this.call<{ result: T }>('Guest.execute_javascript', { code, user_gesture: userGesture });
    return r.result;
  }
  public async insertCSS(css: string): Promise<string> {
    return (await this.call<{ key: string }>('Guest.insert_css', { css })).key;
  }
  public async removeInsertedCSS(key: string): Promise<void> {
    await this.call('Guest.remove_inserted_css', { key });
  }
  /** Results arrive as "found-in-page" events; returns the request id. */
  public async findInPage(text: string, options: FindInPageOptions = {}): Promise<number> {
    const r = await this.call<{ request_id: number }>('Guest.find_in_page', {
      text,
      forward: options.forward ?? true,
      match_case: options.matchCase ?? false,
      find_next: options.findNext ?? false,
    });
    return r.request_id;
  }
  public async stopFindInPage(action: 'clearSelection' | 'keepSelection' | 'activateSelection' = 'clearSelection'): Promise<void> {
    await this.call('Guest.stop_find_in_page', { action });
  }
  public async setZoomFactor(factor: number): Promise<void> {
    this.info.zoom = factor;
    await this.call('Guest.set_zoom_factor', { factor });
  }
  public async setAudioMuted(muted: boolean): Promise<void> {
    this.info.muted = muted;
    await this.call('Guest.set_audio_muted', { muted });
  }
  public async setUserAgent(userAgent: string): Promise<void> {
    await this.call('Guest.set_user_agent', { user_agent: userAgent });
  }
  public async openDevTools(): Promise<void> {
    await this.call('Guest.open_devtools');
  }
  /** Viewport screenshot (Electron returns a NativeImage; this returns base64 with helpers). */
  public async capturePage(format: 'png' | 'jpeg' = 'png'): Promise<CapturedImage> {
    const r = await this.call<GuestCaptureResult>('Guest.capture_page', { format, channel: '' });
    return {
      mimeType: r.mime_type,
      data: r.data,
      size: r.size,
      toDataURL: () => 'data:' + r.mime_type + ';base64,' + r.data,
      toBlob: () => new Blob([base64ToBytes(r.data)], { type: r.mime_type }),
    };
  }
  /** Large captures without base64: the bytes arrive on transfer.ts onBinary(channel) (BinaryTransport). */
  public async capturePageTo(channel: string, format: 'png' | 'jpeg' = 'png'): Promise<GuestCaptureResult> {
    return this.call<GuestCaptureResult>('Guest.capture_page', { format, channel });
  }
  public async print(options: PrintOptions = {} as PrintOptions): Promise<PrintResult> {
    return this.call<PrintResult>('Guest.print', { options });
  }
  public async printToPDF(options: PrintOptions = {} as PrintOptions): Promise<Uint8Array> {
    const r = await this.call<GuestPdfResult>('Guest.print_to_pdf', { path: '', channel: '', options });
    return base64ToBytes(r.data);
  }
  /** Host -> guest message: bosonGuest.on(channel, (event, ...args) => ...) in the guest. */
  public async send(channel: string, ...args: unknown[]): Promise<void> {
    await this.call('Guest.send', { channel, args });
  }
  public override focus(options?: FocusOptions): void {
    super.focus(options);
  }
  public async respondToPermission(requestId: number, allow: boolean): Promise<void> {
    await this.call('Guest.respond_permission', { request_id: requestId, allow });
  }
  public async respondToLogin(requestId: number, credentials: { username: string; password: string } | null): Promise<void> {
    await this.call('Guest.respond_login', {
      request_id: requestId,
      username: credentials?.username ?? '',
      password: credentials?.password ?? '',
      cancel: credentials === null,
    });
  }
  public async respondToCertificateError(requestId: number, allow: boolean): Promise<void> {
    await this.call('Guest.respond_certificate', { request_id: requestId, allow });
  }
  public async downloadAction(downloadId: number, action: 'cancel' | 'pause' | 'resume'): Promise<void> {
    await this.call('Guest.download', { download_id: downloadId, action });
  }

  /** Chrome DevTools Protocol on this guest (CallDevToolsProtocolMethod / event receivers). */
  public readonly cdp = {
    send: async <T = unknown>(method: string, params: object = {}, sessionId = ''): Promise<T> => {
      const r = await this.call<{ result: T }>('Guest.cdp_send', { method, params, session_id: sessionId });
      return r.result;
    },
    on: (event: string, listener: (params: unknown, sessionId: string) => void): Unsubscribe => {
      let set = this.cdpListeners.get(event);
      if (!set) {
        set = new Set();
        this.cdpListeners.set(event, set);
        void this.call('Guest.cdp_subscribe', { events: [event], subscribe: true }).catch(() => undefined);
      }
      set.add(listener);
      return () => {
        const current = this.cdpListeners.get(event);
        current?.delete(listener);
        if (current && current.size === 0) {
          this.cdpListeners.delete(event);
          if (this.id_ !== 0) {
            void boson.call('Guest.cdp_subscribe', { id: this.id_, events: [event], subscribe: false }).catch(() => undefined);
          }
        }
      };
    },
  };
}

/** Registers <boson-webview> (idempotent). Importing this module registers it too. */
export function defineWebViewElement(tag: string = WEBVIEW_TAG): void {
  if (!customElements.get(tag)) {
    customElements.define(tag, tag === WEBVIEW_TAG ? BosonWebViewElement : class extends BosonWebViewElement {});
  }
}

defineWebViewElement();

/** The local CDP endpoint (chromium.connectOverCDP) when the app runs a webview::CdpServer. */
export function cdpEndpoint(): Promise<{ enabled: boolean; http: string; websocket: string }> {
  return boson.call('Guest.cdp_endpoint');
}

declare global {
  interface HTMLElementTagNameMap {
    'boson-webview': BosonWebViewElement;
  }
}
