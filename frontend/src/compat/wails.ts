/**
 * Wails v3 compatibility shim: Call.ByName / Call.Call, Events.On / Once / OnMultiple / Off / OffAll / Emit and
 * generated-bindings style service objects, on top of Boson IPC.
 *
 * Name mapping: "pkg.Service.Method" -> Boson "Service.Method" (the Go package path is dropped; the Boson
 * controller name is the Service, BOSON_CONTROLLER("Service"), and the method is the C++ method name).
 * Boson methods take ONE JSON object, Wails methods take positional args:
 *   0 args             -> {}
 *   1 object arg       -> that object
 *   anything else      -> needs defineBinding(name, { params: ['a', 'b'] }) to name the positions, or
 *                         defineBinding(name, { channel: 'x' }) to route to ElectronIpc::handle("x") with the
 *                         positional args array unchanged.
 */
import { boson, BosonIpcError, type Unsubscribe } from '../boson';

export interface BindingRoute {
  /** Boson "Controller.method" (default: last two segments of the Wails name). */
  method?: string;
  /** Names for positional args; the call sends { [params[i]]: args[i] }. */
  params?: readonly string[];
  /** Route to ElectronIpc.invoke with this channel and the positional args (ignores method/params). */
  channel?: string;
}

const routes = new Map<string, BindingRoute>();

export function defineBinding(name: string, route: BindingRoute): void {
  routes.set(name, route);
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

function byName<T = unknown>(name: string, ...args: unknown[]): Promise<T> {
  try {
    const route = routes.get(name);
    if (route?.channel) {
      return boson.call<T, { channel: string; args: unknown[] }>('ElectronIpc.invoke', { channel: route.channel, args });
    }
    const method = route?.method ?? bosonMethodOf(name);
    if (route?.params) {
      const params: Record<string, unknown> = {};
      route.params.forEach((key, i) => {
        params[key] = args[i];
      });
      return boson.call<T, Record<string, unknown>>(method, params);
    }
    if (args.length === 0) {
      return boson.call<T, Record<string, never>>(method, {});
    }
    if (args.length === 1 && isPlainObject(args[0])) {
      return boson.call<T, Record<string, unknown>>(method, args[0]);
    }
    return Promise.reject(
      new BosonIpcError(`'${name}' was called with positional arguments; register defineBinding('${name}', { params: [...] })`, -32602)
    );
  } catch (err) {
    return Promise.reject(err);
  }
}

export interface CallOptions {
  methodName?: string;
  methodID?: number;
  args?: unknown[];
}

export const Call = {
  ByName: byName,
  /** Wails numeric method IDs come from its code generator; Boson routes by name only. */
  ByID<T = unknown>(methodID: number): Promise<T> {
    return Promise.reject(new BosonIpcError(`Call.ByID(${methodID}) is not supported; use Call.ByName`, -32601));
  },
  Call<T = unknown>(options: CallOptions): Promise<T> {
    if (!options.methodName) {
      return Call.ByID<T>(options.methodID ?? 0);
    }
    return byName<T>(options.methodName, ...(options.args ?? []));
  },
};

// ---- Generated-bindings style --------------------------------------------------------------------------------

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type AnyFn = (...args: any[]) => Promise<unknown>;

/**
 * Builds a service object like the files Wails generates under frontend/bindings/:
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

export default { Call, Events, bindService, defineBinding };
