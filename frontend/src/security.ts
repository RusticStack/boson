import { boson, BosonIpcError } from './boson';

/** JSON-RPC code returned when a capability policy rejects a call (boson::security::kPermissionDeniedCode). */
export const PERMISSION_DENIED_CODE = -32003;

export function isPermissionDenied(err: unknown): boolean {
  return err instanceof BosonIpcError && err.code === PERMISSION_DENIED_CODE;
}

export interface FsStat {
  exists: boolean;
  is_file: boolean;
  is_dir: boolean;
  is_symlink: boolean;
  readonly: boolean;
  size: number;
  modified_ms: number;
  created_ms: number;
  accessed_ms: number;
  path: string;
}

export interface FsEntry {
  name: string;
  path: string;
  is_file: boolean;
  is_dir: boolean;
  is_symlink: boolean;
  size: number;
  modified_ms: number;
}

export interface FsScopeInfo {
  path: string;
  label: string;
  read_only: boolean;
  recursive: boolean;
}

export type FsChangeKind = 'added' | 'removed' | 'modified' | 'renamed_from' | 'renamed_to';

export interface FsChangeBatch {
  id: number;
  changes: { kind: FsChangeKind; path: string }[];
  overflow: boolean;
  closed: boolean;
}

function toBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

function fromBase64(text: string): Uint8Array {
  const binary = atob(text);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    out[i] = binary.charCodeAt(i);
  }
  return out;
}

/** Scoped file system (C++ FsController). Paths outside the granted scopes reject with "permission denied". */
export const fs = {
  readText: async (path: string): Promise<string> =>
    (await boson.call<{ text: string }, { path: string }>('Fs.read_text', { path })).text,
  readBytes: async (path: string): Promise<Uint8Array> =>
    fromBase64((await boson.call<{ data: string }, { path: string }>('Fs.read_bytes', { path })).data),
  writeText: (path: string, text: string, createDirs = false): Promise<void> =>
    boson.call('Fs.write_text', { path, text, create_dirs: createDirs }),
  writeBytes: (path: string, data: Uint8Array, createDirs = false): Promise<void> =>
    boson.call('Fs.write_bytes', { path, data: toBase64(data), create_dirs: createDirs }),
  append: (path: string, text: string, createDirs = false): Promise<void> =>
    boson.call('Fs.append', { path, text, create_dirs: createDirs }),
  exists: async (path: string): Promise<boolean> =>
    (await boson.call<{ value: boolean }, { path: string }>('Fs.exists', { path })).value,
  stat: (path: string): Promise<FsStat> => boson.call<FsStat, { path: string }>('Fs.stat', { path }),
  readDir: async (path: string): Promise<FsEntry[]> =>
    (await boson.call<{ entries: FsEntry[] }, { path: string }>('Fs.read_dir', { path })).entries,
  mkdir: (path: string, recursive = true): Promise<void> => boson.call('Fs.mkdir', { path, recursive }),
  remove: (path: string, recursive = false): Promise<void> => boson.call('Fs.remove', { path, recursive }),
  rename: (from: string, to: string, overwrite = false): Promise<void> =>
    boson.call('Fs.rename', { from, to, overwrite }),
  copy: (from: string, to: string, overwrite = false): Promise<void> => boson.call('Fs.copy', { from, to, overwrite }),
  scopes: async (): Promise<FsScopeInfo[]> => (await boson.call<{ scopes: FsScopeInfo[] }>('Fs.scopes')).scopes,
  grantFromDialog: (options: { title?: string; pick_folder?: boolean; allow_multiple?: boolean; read_only?: boolean } = {}) =>
    boson.call<{ cancelled: boolean; paths: string[] }, typeof options>('Fs.grant_from_dialog', options),
  /** Watches a directory; listener receives batches for this watch only. Resolves to a stop function. */
  async watch(path: string, listener: (batch: FsChangeBatch) => void, recursive = true): Promise<() => Promise<void>> {
    const { id } = await boson.call<{ id: number }, { path: string; recursive: boolean }>('Fs.watch', { path, recursive });
    const off = boson.on<FsChangeBatch>('fs:changed', (batch) => {
      if (batch.id === id) {
        listener(batch);
      }
    });
    return async () => {
      off();
      await boson.call('Fs.unwatch', { id });
    };
  },
};

/** DPAPI-backed encryption bound to the current Windows user (C++ SafeStorageController). */
export const safeStorage = {
  isAvailable: async (): Promise<boolean> => (await boson.call<{ value: boolean }>('SafeStorage.is_available')).value,
  /** Returns the ciphertext as base64. */
  encryptString: async (text: string): Promise<string> =>
    (await boson.call<{ data: string }, { text: string }>('SafeStorage.encrypt_string', { text })).data,
  decryptString: async (data: string): Promise<string> =>
    (await boson.call<{ text: string }, { data: string }>('SafeStorage.decrypt_string', { data })).text,
  encryptBytes: async (bytes: Uint8Array): Promise<string> =>
    (await boson.call<{ data: string }, { data: string }>('SafeStorage.encrypt_bytes', { data: toBase64(bytes) })).data,
  decryptBytes: async (data: string): Promise<Uint8Array> =>
    fromBase64((await boson.call<{ data: string }, { data: string }>('SafeStorage.decrypt_bytes', { data })).data),
};

/** Windows Credential Manager entries of this app (C++ CredentialController). */
export const credentials = {
  set: (key: string, secret: string, username = ''): Promise<void> =>
    boson.call('Credential.set', { key, secret, username }),
  get: async (key: string): Promise<{ username: string; secret: string } | null> => {
    const res = await boson.call<{ found: boolean; username: string; secret: string }, { key: string }>('Credential.get', { key });
    return res.found ? { username: res.username, secret: res.secret } : null;
  },
  remove: async (key: string): Promise<boolean> =>
    (await boson.call<{ value: boolean }, { key: string }>('Credential.remove', { key })).value,
  list: async (): Promise<string[]> => (await boson.call<{ keys: string[] }>('Credential.list')).keys,
};
