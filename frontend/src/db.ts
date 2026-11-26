import { boson } from './boson';

/** Request payloads are plain JSON objects matching the controller's request struct. */
type P = Record<string, unknown>;

/** Mirrors boson::db::KvEntry, with the value parsed. */
export interface KvEntry<T = unknown> {
  key: string;
  value: T;
  updated_ms: number;
}

export interface DbInfo {
  backend: 'sqlite' | 'zephyr';
  version: string;
  namespaces: string[];
  read_only_namespaces: string[];
}

/**
 * Namespaced JSON key/value storage (boson::ipc::DbController over SQLite or ZephyrDB). Only namespaces the
 * app granted in C++ are reachable.
 */
export class KvNamespace {
  constructor(public readonly ns: string) {}

  async get<T = unknown>(key: string): Promise<T | undefined> {
    const r = await boson.call<{ found: boolean; value: string }, P>('Db.get', { ns: this.ns, key });
    return r.found ? (JSON.parse(r.value) as T) : undefined;
  }

  set<T>(key: string, value: T): Promise<void> {
    return boson.call<void, P>('Db.set', { ns: this.ns, key, value: JSON.stringify(value) ?? 'null' });
  }

  remove(key: string): Promise<boolean> {
    return boson.call<{ value: boolean }, P>('Db.remove', { ns: this.ns, key }).then((r) => r.value);
  }

  keys(prefix = '', limit?: number): Promise<string[]> {
    return boson.call<{ keys: string[] }, P>('Db.keys', { ns: this.ns, prefix, limit: limit ?? null }).then((r) => r.keys);
  }

  async entries<T = unknown>(prefix = '', limit?: number): Promise<KvEntry<T>[]> {
    const r = await boson.call<{ entries: { key: string; value: string; updated_ms: number }[] }, P>('Db.entries', {
      ns: this.ns,
      prefix,
      limit: limit ?? null,
    });
    return r.entries.map((e) => ({ key: e.key, value: JSON.parse(e.value) as T, updated_ms: e.updated_ms }));
  }

  clear(): Promise<number> {
    return boson.call<{ count: number }, P>('Db.clear', { ns: this.ns }).then((r) => r.count);
  }

  count(): Promise<number> {
    return boson.call<{ count: number }, P>('Db.count', { ns: this.ns }).then((r) => r.count);
  }
}

export const db = {
  namespace(ns: string): KvNamespace {
    return new KvNamespace(ns);
  },
  info(): Promise<DbInfo> {
    return boson.call<DbInfo, P>('Db.info');
  },
};
