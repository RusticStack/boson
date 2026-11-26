import { boson, type Unsubscribe } from './boson';

/** Mirrors boson::ipc::UpdateCheckResponse. */
export interface UpdateCheck {
  ok: boolean;
  error: string;
  available: boolean;
  newer: boolean;
  mandatory: boolean;
  in_rollout: boolean;
  current_version: string;
  version: string;
  notes: string;
  kind: 'msi' | 'zip' | 'exe' | 'auto';
  size?: number;
}

export interface UpdateProgress {
  version: string;
  received: number;
  total?: number;
  fraction: number;
  bytes_per_second: number;
}

export interface UpdateResult {
  version: string;
  ok: boolean;
  cancelled: boolean;
  error: string;
}

export interface UpdateStatus {
  state: 'idle' | 'checking' | 'available' | 'up_to_date' | 'downloading' | 'ready' | 'scheduled' | 'error';
  version: string;
  error: string;
  received: number;
  total?: number;
}

export interface LastUpdateResult {
  has_result: boolean;
  version: string;
  success: boolean;
  rolled_back: boolean;
  error: string;
  finished_at: string;
}

export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error' | 'fatal';

/** Typed wrappers over the Updater controller (boson::ipc::UpdaterController). */
export const updater = {
  check(): Promise<UpdateCheck> {
    return boson.call<UpdateCheck>('Updater.check');
  },

  status(): Promise<UpdateStatus> {
    return boson.call<UpdateStatus>('Updater.status');
  },

  cancel(): Promise<void> {
    return boson.call<void>('Updater.cancel');
  },

  lastResult(): Promise<LastUpdateResult> {
    return boson.call<LastUpdateResult>('Updater.last_result');
  },

  /** Schedules the install; with quitNow the app exits so the helper can install (and relaunch) at once. */
  install(options: { relaunch?: boolean; quitNow?: boolean } = {}): Promise<{ scheduled: boolean; error: string }> {
    return boson.call<{ scheduled: boolean; error: string }, { relaunch: boolean; quit_now: boolean }>('Updater.install', {
      relaunch: options.relaunch ?? true,
      quit_now: options.quitNow ?? true,
    });
  },

  /**
   * Starts the download and resolves when it is downloaded and verified (rejects on failure or cancel).
   * onProgress receives the "updater:progress" events.
   */
  download(onProgress?: (progress: UpdateProgress) => void): Promise<UpdateResult> {
    return new Promise<UpdateResult>((resolve, reject) => {
      const subscriptions: Unsubscribe[] = [];
      const done = (): void => {
        for (const off of subscriptions) off();
      };
      if (onProgress) {
        subscriptions.push(boson.on<UpdateProgress>('updater:progress', onProgress));
      }
      subscriptions.push(
        boson.on<UpdateResult>('updater:downloaded', (result) => {
          done();
          resolve(result);
        }),
      );
      subscriptions.push(
        boson.on<UpdateResult>('updater:error', (result) => {
          done();
          reject(new Error(result.cancelled ? 'Update download cancelled' : result.error));
        }),
      );
      boson
        .call<{ started: boolean; error: string }>('Updater.download')
        .then((started) => {
          if (!started.started) {
            done();
            reject(new Error(started.error));
          }
        })
        .catch((err: unknown) => {
          done();
          reject(err);
        });
    });
  },
};

/** Writes one line into the app's rotating log file (boson::ipc::LogController). Never throws. */
export function log(level: LogLevel, message: string, category = 'ui'): void {
  if (!boson.isAvailable()) {
    return;
  }
  void boson.call<void, { level: LogLevel; category: string; message: string }>('Log.write', { level, category, message }).catch(() => {});
}
