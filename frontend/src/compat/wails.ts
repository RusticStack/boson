/**
 * Wails v3 compatibility shim for `@wailsio/runtime`: Call (ByName / ByID / Call), CancellablePromise, Create,
 * Events, Window and Application, on top of Boson IPC. Alias it in vite.config.ts and the files that
 * `wails3 generate bindings` writes build and run unchanged:
 *   resolve: { alias: { '@wailsio/runtime': '/src/compat/wails.ts' } }
 *
 * Name mapping: "pkg.Service.Method" -> Boson "Service.Method" (the Go package path is dropped; the Boson
 * controller name is the Service, BOSON_CONTROLLER("Service"), and the method is its glz::meta key).
 *
 * Method IDs: generated bindings call `Call.ByID(fnv1a32("pkg.Service.Method"), ...args)`. Register the names
 * once at startup with registerBindingPackage('main') (every method boson_tsgen knows, under that Go package),
 * registerBindingNames([...]) or registerBindingIds({ id: name }) (for `//wails:id` overrides).
 *
 * Positional args: Boson methods take ONE request struct, Wails methods take positional args. boson_tsgen
 * writes the request struct's member names per method to generated/boson-method-params.ts, so
 * `Service.Method(a, b)` sends `{ [names[0]]: a, [names[1]]: b }`. Without generated names:
 *   0 args             -> {}
 *   1 object arg       -> that object
 *   anything else      -> needs defineBinding(name, { params: ['a', 'b'] }) to name the positions, or
 *                         defineBinding(name, { channel: 'x' }) to route to ElectronIpc::handle("x") with the
 *                         positional args array unchanged.
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';
import { bosonMethodParams } from '../generated/boson-method-params';

// ---- CancellablePromise -------------------------------------------------------------------------------------

/** Rejection reason of a cancelled CancellablePromise. */
export class CancelError extends Error {
  constructor(message?: string, options?: { cause?: unknown }) {
    super(message, options);
    this.name = 'CancelError';
  }
}

export type CancellablePromiseResolver<T> = (value: T | PromiseLike<T>) => void;
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type CancellablePromiseRejector = (reason?: any) => void;
export type CancellablePromiseExecutor<T> = (resolve: CancellablePromiseResolver<T>, reject: CancellablePromiseRejector) => void;
/** Runs when the promise is cancelled while still pending; may return a promise cancel() waits for. */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type CancellablePromiseCanceller = (cause?: any) => void | PromiseLike<void>;

export interface CancellablePromiseLike<T> {
  then<TResult1 = T, TResult2 = never>(
    onfulfilled?: ((value: T) => TResult1 | PromiseLike<TResult1>) | null,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    onrejected?: ((reason: any) => TResult2 | PromiseLike<TResult2>) | null
  ): CancellablePromiseLike<TResult1 | TResult2>;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  cancel(cause?: any): void | PromiseLike<void>;
}

export interface CancellablePromiseWithResolvers<T> {
  promise: CancellablePromise<T>;
  resolve: CancellablePromiseResolver<T>;
  reject: CancellablePromiseRejector;
  oncancelled: CancellablePromiseCanceller | null;
}

interface SettleState {
  done: boolean;
  reject: CancellablePromiseRejector;
}

/**
 * A Promise with cancel(), shaped like @wailsio/runtime's. cancel() rejects a pending promise with a
 * CancelError, runs its oncancelled callback and cancels the promise it was chained from (then / catch /
 * finally keep the class). Boson cannot abort a C++ handler that is already running: cancelling a call only
 * rejects it on the JS side and the late response is dropped.
 */
export class CancellablePromise<T> extends Promise<T> implements CancellablePromiseLike<T> {
  #state: SettleState;
  #oncancelled: CancellablePromiseCanceller | undefined;
  #parent: CancellablePromise<unknown> | undefined;

  constructor(executor: CancellablePromiseExecutor<T>, oncancelled?: CancellablePromiseCanceller) {
    let outerResolve: CancellablePromiseResolver<T> = () => {};
    let outerReject: CancellablePromiseRejector = () => {};
    super((resolve, reject) => {
      outerResolve = resolve;
      outerReject = reject;
    });
    const state: SettleState = { done: false, reject: outerReject };
    this.#state = state;
    this.#oncancelled = oncancelled;
    this.#parent = undefined;
    const resolve: CancellablePromiseResolver<T> = (value) => {
      if (!state.done) {
        state.done = true;
        outerResolve(value);
      }
    };
    const reject: CancellablePromiseRejector = (reason) => {
      if (!state.done) {
        state.done = true;
        outerReject(reason);
      }
    };
    try {
      executor(resolve, reject);
    } catch (err) {
      reject(err);
    }
  }

  override then<TResult1 = T, TResult2 = never>(
    onfulfilled?: ((value: T) => TResult1 | PromiseLike<TResult1>) | null,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    onrejected?: ((reason: any) => TResult2 | PromiseLike<TResult2>) | null
  ): CancellablePromise<TResult1 | TResult2> {
    const child = super.then(onfulfilled, onrejected) as CancellablePromise<TResult1 | TResult2>;
    if (child instanceof CancellablePromise) {
      child.#parent = this as CancellablePromise<unknown>;
    }
    return child;
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  override catch<TResult = never>(onrejected?: ((reason: any) => TResult | PromiseLike<TResult>) | null): CancellablePromise<T | TResult> {
    return this.then(undefined, onrejected);
  }

  override finally(onfinally?: (() => void) | null): CancellablePromise<T> {
    return super.finally(onfinally) as CancellablePromise<T>;
  }

  /** Cancels this promise (if still pending) and the chain it came from; resolves once oncancelled settled. */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  cancel(cause?: any): CancellablePromise<void> {
    const pending: Promise<unknown>[] = [];
    if (!this.#state.done) {
      this.#state.done = true;
      const handler = this.#oncancelled;
      if (handler) {
        try {
          const ret = handler(cause);
          if (ret) {
            pending.push(Promise.resolve(ret));
          }
        } catch (err) {
          pending.push(Promise.reject(err));
        }
      }
      this.#state.reject(new CancelError('Promise cancelled.', { cause }));
    }
    if (this.#parent) {
      pending.push(this.#parent.cancel(cause));
    }
    return new CancellablePromise<void>((resolve) => {
      void Promise.allSettled(pending).then(() => resolve());
    });
  }

  /** Cancels this promise when signal aborts. */
  cancelOn(signal: AbortSignal): CancellablePromise<T> {
    if (signal.aborted) {
      void this.cancel(signal.reason);
    } else {
      signal.addEventListener('abort', () => void this.cancel(signal.reason), { once: true });
    }
    return this;
  }

  static withResolvers<T>(): CancellablePromiseWithResolvers<T> {
    let resolve: CancellablePromiseResolver<T> = () => {};
    let reject: CancellablePromiseRejector = () => {};
    const promise = new CancellablePromise<T>((res, rej) => {
      resolve = res;
      reject = rej;
    });
    return {
      promise,
      resolve,
      reject,
      get oncancelled(): CancellablePromiseCanceller | null {
        return promise.#oncancelled ?? null;
      },
      set oncancelled(handler: CancellablePromiseCanceller | null) {
        promise.#oncancelled = handler ?? undefined;
      },
    };
  }

  /** An already-cancelled promise. */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  static cancel<T = never>(cause?: any): CancellablePromise<T> {
    const promise = new CancellablePromise<T>(() => {});
    void promise.cancel(cause);
    return promise;
  }

  /** A promise that is cancelled after milliseconds. */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  static timeout<T = never>(milliseconds: number, cause?: any): CancellablePromise<T> {
    const promise = new CancellablePromise<T>(() => {});
    setTimeout(() => void promise.cancel(cause), milliseconds);
    return promise;
  }

  static sleep(milliseconds: number): CancellablePromise<void>;
  static sleep<T>(milliseconds: number, value: T): CancellablePromise<T>;
  static sleep<T = void>(milliseconds: number, value?: T): CancellablePromise<T> {
    return new CancellablePromise<T>((resolve) => {
      setTimeout(() => resolve(value as T), milliseconds);
    });
  }
}

function cancellable<T>(promise: Promise<T>): CancellablePromise<T> {
  return new CancellablePromise<T>((resolve, reject) => {
    promise.then(resolve, reject);
  });
}

function rejected<T>(reason: unknown): CancellablePromise<T> {
  return new CancellablePromise<T>((_resolve, reject) => reject(reason));
}

// ---- Create (model constructors used by the generated bindings) --------------------------------------------

/* eslint-disable @typescript-eslint/no-explicit-any */
function createAny<T = any>(source: any): T {
  return source;
}

function createByteSlice(source: any): string {
  return source == null ? '' : source;
}

function createArray<T = any>(element: (source: any) => T): (source: any) => T[] {
  if (element === createAny) {
    return (source) => (source === null ? [] : source);
  }
  return (source) => {
    if (source === null) {
      return [];
    }
    for (let i = 0; i < source.length; i++) {
      source[i] = element(source[i]);
    }
    return source;
  };
}

function createMap<K extends PropertyKey = any, V = any>(_key: (source: any) => K, value: (source: any) => V): (source: any) => Record<K, V> {
  if (value === createAny) {
    return (source) => (source === null ? {} : source);
  }
  return (source) => {
    if (source === null) {
      return {};
    }
    for (const key in source) {
      source[key] = value(source[key]);
    }
    return source;
  };
}

function createNullable<T = any>(element: (source: any) => T): (source: any) => T | null {
  if (element === createAny) {
    return createAny;
  }
  return (source) => (source === null ? null : element(source));
}

function createStruct(createField: Record<string, (source: any) => any>): <U extends Record<string, any> = any>(source: any) => U {
  let allAny = true;
  for (const name in createField) {
    if (createField[name] !== createAny) {
      allAny = false;
      break;
    }
  }
  if (allAny) {
    return createAny;
  }
  return (source) => {
    for (const name in createField) {
      if (name in source) {
        source[name] = createField[name](source[name]);
      }
    }
    return source;
  };
}

function createDateFromTime(source: any): Date {
  return new Date(source);
}

/** Same functions and signatures as @wailsio/runtime's Create namespace (an internal of its generator). */
export const Create = {
  Any: createAny,
  ByteSlice: createByteSlice,
  Array: createArray,
  Map: createMap,
  Nullable: createNullable,
  Struct: createStruct,
  DateFromTime: createDateFromTime,
  /** Event name -> payload creation function; the generated events/ module freezes it. */
  Events: {} as Record<string, (source: any) => any>,
};
/* eslint-enable @typescript-eslint/no-explicit-any */

// ---- Routing --------------------------------------------------------------------------------------------------

export interface BindingRoute {
  /** Boson "Controller.method" (default: last two segments of the Wails name). */
  method?: string;
  /** Names for positional args; the call sends { [params[i]]: args[i] }. */
  params?: readonly string[];
  /** Route to ElectronIpc.invoke with this channel and the positional args (ignores method/params). */
  channel?: string;
}

/** Positional parameter names per Boson "Controller.method"; null = the parameter is not an object. */
export type MethodParams = Readonly<Record<string, readonly string[] | null>>;

const routes = new Map<string, BindingRoute>();
const methodParams = new Map<string, readonly string[] | null>();
const bindingIds = new Map<number, string>();

export function defineBinding(name: string, route: BindingRoute): void {
  routes.set(name, route);
}

/** Adds positional parameter names (boson_tsgen's method-params output, or your own controllers'). */
export function registerBindingParams(params: MethodParams): void {
  for (const [method, names] of Object.entries(params)) {
    methodParams.set(method, names);
  }
}

registerBindingParams(bosonMethodParams);

/** Wails' method ID: 32-bit FNV-1a of the UTF-8 bytes of "pkg.Service.Method" (internal/hash/fnv.go). */
export function bindingId(qualifiedName: string): number {
  let hash = 0x811c9dc5;
  for (const byte of new TextEncoder().encode(qualifiedName)) {
    hash ^= byte;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash >>> 0;
}

/** Explicit ID -> "pkg.Service.Method" entries, e.g. for methods with a `//wails:id` directive. */
export function registerBindingIds(ids: Readonly<Record<number | string, string>>): void {
  for (const [id, name] of Object.entries(ids)) {
    bindingIds.set(Number(id), name);
  }
}

/** Registers the Wails ID of every "pkg.Service.Method" name (e.g. from `wails3 generate bindings -names`). */
export function registerBindingNames(names: Iterable<string>): void {
  for (const name of names) {
    bindingIds.set(bindingId(name), name);
  }
}

/**
 * Registers IDs for "goPackage.Controller.method" of every Boson method (default: every method in
 * boson-method-params.ts plus registerBindingParams()), i.e. what the Go package that held the services
 * was called ("main" for package main, else its import path).
 */
export function registerBindingPackage(goPackage: string, methods: Iterable<string> = methodParams.keys()): void {
  const names: string[] = [];
  for (const method of methods) {
    names.push(`${goPackage}.${method}`);
  }
  registerBindingNames(names);
}

function bosonMethodOf(name: string): string {
  const parts = name.split('.').filter((p) => p.length > 0);
  if (parts.length < 2) {
    throw new BosonIpcError(`Wails binding name '${name}' must look like 'pkg.Service.Method'`, -32601);
  }
  return `${parts[parts.length - 2]}.${parts[parts.length - 1]}`;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function positional(names: readonly string[], args: readonly unknown[]): Record<string, unknown> {
  const params: Record<string, unknown> = {};
  const count = Math.min(names.length, args.length);
  for (let i = 0; i < count; i++) {
    params[names[i]] = args[i];
  }
  return params;
}

/** Boson params for a Wails call, or an Error to reject with. */
function paramsFor(name: string, method: string, route: BindingRoute | undefined, args: readonly unknown[]): unknown {
  if (route?.params) {
    return positional(route.params, args);
  }
  const names = methodParams.get(method);
  if (names !== undefined) {
    if (names === null) {
      return args.length === 0 ? {} : args[0];
    }
    if (args.length > names.length) {
      return new BosonIpcError(`'${name}' takes ${names.length} argument(s), got ${args.length}`, -32602);
    }
    // A single object that already is the request (Boson style) passes through unchanged.
    const passThrough = args.length === 1 && isPlainObject(args[0]) && (names.length !== 1 || names[0] in args[0]);
    return passThrough ? args[0] : positional(names, args);
  }
  if (args.length === 0) {
    return {};
  }
  if (args.length === 1 && isPlainObject(args[0])) {
    return args[0];
  }
  return new BosonIpcError(`'${name}' was called with positional arguments; register defineBinding('${name}', { params: [...] })`, -32602);
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function byName<T = any>(name: string, ...args: unknown[]): CancellablePromise<T> {
  try {
    const route = routes.get(name);
    if (route?.channel) {
      return cancellable(boson.call<T, { channel: string; args: unknown[] }>('ElectronIpc.invoke', { channel: route.channel, args }));
    }
    const method = route?.method ?? bosonMethodOf(name);
    const params = paramsFor(name, method, route, args);
    if (params instanceof Error) {
      return rejected(params);
    }
    return cancellable(boson.call<T, unknown>(method, params));
  } catch (err) {
    return rejected(err);
  }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function byId<T = any>(methodID: number, ...args: unknown[]): CancellablePromise<T> {
  const name = bindingIds.get(methodID >>> 0);
  if (name === undefined) {
    return rejected(
      new BosonIpcError(`Unknown Wails method ID ${methodID}; register it with registerBindingPackage / registerBindingNames / registerBindingIds`, -32601)
    );
  }
  return byName<T>(name, ...args);
}

export interface CallOptions {
  methodName?: string;
  methodID?: number;
  args?: unknown[];
}

export const Call = {
  ByName: byName,
  ByID: byId,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  Call<T = any>(options: CallOptions): CancellablePromise<T> {
    if (options.methodName !== undefined) {
      return byName<T>(options.methodName, ...(options.args ?? []));
    }
    if (options.methodID !== undefined) {
      return byId<T>(options.methodID, ...(options.args ?? []));
    }
    return rejected(new BosonIpcError('Call.Call needs methodName or methodID', -32601));
  },
};

// ---- Generated-bindings style --------------------------------------------------------------------------------

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type AnyFn = (...args: any[]) => Promise<unknown>;

/**
 * Builds a service object by hand, like the files Wails generates under frontend/bindings/:
 *   const GreetService = bindService<{ Greet(req: { name: string }): Promise<string> }>('main.GreetService', ['Greet']);
 *   await GreetService.Greet({ name: 'Boson' });
 */
export function bindService<S extends Record<string, AnyFn>>(qualifiedService: string, methods: readonly (keyof S & string)[]): S {
  const service: Record<string, AnyFn> = {};
  for (const method of methods) {
    service[method] = (...args: unknown[]) => byName(`${qualifiedService}.${method}`, ...args);
  }
  return Object.freeze(service) as S;
}

// ---- Window / Application (SystemController, "Boson.*") ---------------------------------------------------------

interface BoolValue {
  value: boolean;
}

function system<T = void>(method: string, params: Record<string, unknown> = {}): CancellablePromise<T> {
  return cancellable(boson.call<T, Record<string, unknown>>(`Boson.${method}`, params));
}

function systemFlag(method: string): CancellablePromise<boolean> {
  return system<BoolValue>(method).then((r) => r.value);
}

function done(): void {}

/** The Wails Window methods Boson provides (the rest have no SystemController counterpart). */
export interface WailsWindow {
  Get(name: string): WailsWindow;
  Minimise(): CancellablePromise<void>;
  UnMinimise(): CancellablePromise<void>;
  Maximise(): CancellablePromise<void>;
  UnMaximise(): CancellablePromise<void>;
  ToggleMaximise(): CancellablePromise<void>;
  Restore(): CancellablePromise<void>;
  IsMaximised(): CancellablePromise<boolean>;
  IsMinimised(): CancellablePromise<boolean>;
  IsFullscreen(): CancellablePromise<boolean>;
  Fullscreen(): CancellablePromise<void>;
  UnFullscreen(): CancellablePromise<void>;
  ToggleFullscreen(): CancellablePromise<void>;
  Close(): CancellablePromise<void>;
  SetTitle(title: string): CancellablePromise<void>;
  Center(): CancellablePromise<void>;
  SetAlwaysOnTop(alwaysOnTop: boolean): CancellablePromise<void>;
  SetMinSize(width: number, height: number): CancellablePromise<void>;
  SetZoom(zoom: number): CancellablePromise<void>;
  ZoomReset(): CancellablePromise<void>;
  Reload(): CancellablePromise<void>;
  ForceReload(): CancellablePromise<void>;
  OpenDevTools(): CancellablePromise<void>;
}

/**
 * The current window, mapped onto SystemController. Boson has one SystemController per WebView, so Get()
 * returns this same object; methods without a SystemController counterpart are not provided.
 */
export const Window: WailsWindow = {
  Get(_name: string): WailsWindow {
    return Window;
  },
  Minimise: (): CancellablePromise<void> => system('minimize'),
  UnMinimise: (): CancellablePromise<void> => system('restore'),
  Maximise: (): CancellablePromise<void> =>
    systemFlag('is_maximized').then((max) => (max ? undefined : system<BoolValue>('maximize_or_restore').then(done))),
  UnMaximise: (): CancellablePromise<void> =>
    systemFlag('is_maximized').then((max) => (max ? system<BoolValue>('maximize_or_restore').then(done) : undefined)),
  ToggleMaximise: (): CancellablePromise<void> => system<BoolValue>('maximize_or_restore').then(done),
  /** Leaves fullscreen, or restores a minimized / maximized window to its normal size. */
  Restore: (): CancellablePromise<void> =>
    systemFlag('is_fullscreen').then((full) => (full ? system('set_fullscreen', { enabled: false }) : system('restore'))),
  IsMaximised: (): CancellablePromise<boolean> => systemFlag('is_maximized'),
  IsMinimised: (): CancellablePromise<boolean> => systemFlag('is_minimized'),
  IsFullscreen: (): CancellablePromise<boolean> => systemFlag('is_fullscreen'),
  Fullscreen: (): CancellablePromise<void> => system('set_fullscreen', { enabled: true }),
  UnFullscreen: (): CancellablePromise<void> => system('set_fullscreen', { enabled: false }),
  ToggleFullscreen: (): CancellablePromise<void> =>
    systemFlag('is_fullscreen').then((full) => system('set_fullscreen', { enabled: !full })),
  Close: (): CancellablePromise<void> => system('close'),
  SetTitle: (title: string): CancellablePromise<void> => system('set_title', { title }),
  Center: (): CancellablePromise<void> => system('center'),
  SetAlwaysOnTop: (alwaysOnTop: boolean): CancellablePromise<void> => system('set_always_on_top', { enabled: alwaysOnTop }),
  SetMinSize: (width: number, height: number): CancellablePromise<void> => system('set_min_size', { width, height }),
  SetZoom: (zoom: number): CancellablePromise<void> => system('set_zoom', { factor: zoom }),
  ZoomReset: (): CancellablePromise<void> => system('set_zoom', { factor: 1 }),
  Reload: (): CancellablePromise<void> => system('reload'),
  ForceReload: (): CancellablePromise<void> => system('reload'),
  OpenDevTools: (): CancellablePromise<void> => system('open_devtools'),
};

export const Application = {
  /** SystemController::quit: the app's quit handler (set_quit_handler), else closes the window. */
  Quit: (): CancellablePromise<void> => system('quit'),
};

// ---- Events ---------------------------------------------------------------------------------------------------

export interface WailsEvent<T = unknown> {
  name: string;
  data: T;
  sender?: string;
}

export type WailsEventCallback<T = unknown> = (event: WailsEvent<T>) => void;

const eventSubscriptions = new Map<string, Set<Unsubscribe>>();

function track(name: string, off: Unsubscribe): Unsubscribe {
  let set = eventSubscriptions.get(name);
  if (!set) {
    set = new Set();
    eventSubscriptions.set(name, set);
  }
  set.add(off);
  return () => {
    off();
    const current = eventSubscriptions.get(name);
    current?.delete(off);
    if (current && current.size === 0) {
      eventSubscriptions.delete(name);
    }
  };
}

function onMultiple<T = unknown>(name: string, callback: WailsEventCallback<T>, maxCallbacks: number): () => void {
  let remaining = maxCallbacks;
  let stop: Unsubscribe = () => {};
  const off = boson.on<T>(name, (data: T) => {
    if (maxCallbacks > 0) {
      remaining -= 1;
      if (remaining <= 0) {
        stop();
      }
    }
    callback({ name, data });
  });
  stop = track(name, off);
  return stop;
}

export const Events = {
  On<T = unknown>(name: string, callback: WailsEventCallback<T>): () => void {
    return onMultiple(name, callback, -1);
  },
  Once<T = unknown>(name: string, callback: WailsEventCallback<T>): () => void {
    return onMultiple(name, callback, 1);
  },
  OnMultiple: onMultiple,
  /** Removes every listener of each named event. */
  Off(name: string, ...additionalNames: string[]): void {
    for (const event of [name, ...additionalNames]) {
      for (const off of eventSubscriptions.get(event) ?? []) {
        off();
      }
      eventSubscriptions.delete(event);
    }
  },
  OffAll(): void {
    for (const name of [...eventSubscriptions.keys()]) {
      Events.Off(name);
    }
  },
  /** Reaches C++ ElectronIpc::on_event listeners and every attached WebView. */
  async Emit<T = unknown>(nameOrEvent: string | WailsEvent<T>, data?: T): Promise<void> {
    const event = typeof nameOrEvent === 'string' ? nameOrEvent : nameOrEvent.name;
    const payload = typeof nameOrEvent === 'string' ? data : nameOrEvent.data;
    await boson.call('ElectronIpc.emit', { event, payload: payload === undefined ? null : payload, broadcast: true });
  },
};

export default { Application, Call, CancellablePromise, Create, Events, Window, bindService, defineBinding };
