// Typed helpers for the desktop controllers (Clipboard, Shell, Screen, Shortcut) and their events.
// Works before the generated bindings include these controllers: every call goes through boson.call().
import { boson, BosonIpcError, type Unsubscribe } from './boson';

export interface ScreenRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface ScreenPoint {
  x: number;
  y: number;
}

export interface Display {
  id: string;
  device_name: string;
  label: string;
  bounds: ScreenRect;
  work_area: ScreenRect;
  dpi: number;
  scale_factor: number;
  primary: boolean;
  refresh_rate: number;
  rotation: number;
  bits_per_pixel: number;
}

export interface ClipboardChangedEvent {
  sequence_number: number;
  has_text: boolean;
  has_html: boolean;
  has_image: boolean;
  has_files: boolean;
}

export interface ScreenChangedEvent {
  reason: 'display-change' | 'work-area' | 'dpi' | 'settings';
  displays: Display[];
  added: string[];
  removed: string[];
  changed: string[];
}

export interface ShortcutTriggeredEvent {
  id: number;
  accelerator: string;
}

export interface ClipboardHtml {
  available: boolean;
  html: string;
  fragment: string;
  source_url: string;
}

export interface ClipboardImage {
  available: boolean;
  png_base64: string;
  width: number;
  height: number;
}

export interface ClipboardFormat {
  id: number;
  name: string;
}

export type KnownFolderName =
  | 'home' | 'desktop' | 'documents' | 'downloads' | 'music' | 'pictures' | 'videos' | 'appData'
  | 'localAppData' | 'temp' | 'recent' | 'startMenu' | 'programs' | 'startup' | 'programFiles'
  | 'public' | 'savedGames' | 'fonts' | 'exe';

interface ShortcutRegisterResponse {
  ok: boolean;
  id: number;
  accelerator: string;
  error: '' | 'invalid' | 'already_registered' | 'conflict' | 'system';
  message: string;
}

/** Thrown by shortcuts.register(); kind tells a conflict with another app apart from a bad accelerator. */
export class ShortcutError extends Error {
  constructor(message: string, public readonly kind: ShortcutRegisterResponse['error'], public readonly accelerator: string) {
    super(message);
    this.name = 'ShortcutError';
  }
}

const empty = {} as Record<string, never>;
const call = <R = void, P = unknown>(method: string, params: P): Promise<R> => boson.call<R, P>(method, params);

export const clipboard = {
  async readText(): Promise<string | null> {
    const r = await call<{ text?: string | null }>('Clipboard.read_text', empty);
    return r.text ?? null;
  },
  writeText(text: string, options: { excludeFromHistory?: boolean } = {}): Promise<void> {
    return call('Clipboard.write_text', { text, exclude_from_history: options.excludeFromHistory ?? false });
  },
  readHtml(): Promise<ClipboardHtml> {
    return call<ClipboardHtml>('Clipboard.read_html', empty);
  },
  writeHtml(html: string, options: { text?: string; sourceUrl?: string; excludeFromHistory?: boolean } = {}): Promise<void> {
    return call('Clipboard.write_html', {
      html,
      text: options.text ?? null,
      source_url: options.sourceUrl ?? '',
      exclude_from_history: options.excludeFromHistory ?? false,
    });
  },
  /** PNG as base64; null when the clipboard holds no image. */
  async readImage(): Promise<ClipboardImage | null> {
    const r = await call<ClipboardImage>('Clipboard.read_image', empty);
    return r.available ? r : null;
  },
  /** Accepts raw base64 or a data:image/png;base64,... URL (e.g. canvas.toDataURL()). */
  writeImage(pngBase64: string, options: { excludeFromHistory?: boolean } = {}): Promise<void> {
    return call('Clipboard.write_image', { png_base64: pngBase64, exclude_from_history: options.excludeFromHistory ?? false });
  },
  async readFiles(): Promise<string[]> {
    return (await call<{ paths: string[] }>('Clipboard.read_files', empty)).paths;
  },
  writeFiles(paths: string[]): Promise<void> {
    return call('Clipboard.write_files', { paths });
  },
  clear(): Promise<void> {
    return call('Clipboard.clear', empty);
  },
  async formats(): Promise<ClipboardFormat[]> {
    return (await call<{ formats: ClipboardFormat[] }>('Clipboard.formats', empty)).formats;
  },
  async has(format: 'text' | 'html' | 'image' | 'files' | (string & {})): Promise<boolean> {
    return (await call<{ value: boolean }>('Clipboard.has', { format })).value;
  },
  onChanged(listener: (e: ClipboardChangedEvent) => void): Unsubscribe {
    return boson.on<ClipboardChangedEvent>('clipboard:changed', listener);
  },
};

export const shell = {
  /** http, https and mailto by default (the C++ side owns the allow-list). */
  openExternal(url: string): Promise<void> {
    return call('Shell.open_external', { url });
  },
  openPath(path: string): Promise<void> {
    return call('Shell.open_path', { path });
  },
  showItemInFolder(path: string): Promise<void> {
    return call('Shell.show_item_in_folder', { path });
  },
  moveToTrash(path: string): Promise<void> {
    return call('Shell.move_to_trash', { path });
  },
  async getPath(name: KnownFolderName): Promise<string> {
    return (await call<{ path: string }>('Shell.get_known_folder', { name })).path;
  },
};

export const screen = {
  async getAllDisplays(): Promise<Display[]> {
    return (await call<{ displays: Display[] }>('Screen.get_all_displays', empty)).displays;
  },
  getPrimaryDisplay(): Promise<Display> {
    return call<Display>('Screen.get_primary_display', empty);
  },
  getDisplayNearestPoint(point: ScreenPoint): Promise<Display> {
    return call<Display, ScreenPoint>('Screen.get_display_nearest_point', point);
  },
  getDisplayMatching(rect: ScreenRect): Promise<Display> {
    return call<Display, ScreenRect>('Screen.get_display_matching', rect);
  },
  getWindowDisplay(): Promise<Display> {
    return call<Display>('Screen.get_window_display', empty);
  },
  getCursorScreenPoint(): Promise<ScreenPoint> {
    return call<ScreenPoint>('Screen.get_cursor_screen_point', empty);
  },
  onChanged(listener: (e: ScreenChangedEvent) => void): Unsubscribe {
    return boson.on<ScreenChangedEvent>('screen:changed', listener);
  },
};

const shortcutListeners = new Map<number, () => void>();
let shortcutSubscription: Unsubscribe | null = null;

export const shortcuts = {
  /**
   * Registers a global shortcut ("CommandOrControl+Shift+K"). Resolves with its id; rejects with ShortcutError
   * (kind 'conflict' when another application owns the combination).
   */
  async register(accelerator: string, listener?: () => void): Promise<number> {
    const r = await call<ShortcutRegisterResponse>('Shortcut.register', { accelerator });
    if (!r.ok) {
      throw new ShortcutError(r.message, r.error, r.accelerator);
    }
    if (listener) {
      shortcutListeners.set(r.id, listener);
      shortcutSubscription ??= boson.on<ShortcutTriggeredEvent>('shortcut:triggered', (e) => shortcutListeners.get(e.id)?.());
    }
    return r.id;
  },
  async unregister(id: number): Promise<boolean> {
    shortcutListeners.delete(id);
    return (await call<{ value: boolean }>('Shortcut.unregister', { id })).value;
  },
  async unregisterAll(): Promise<void> {
    shortcutListeners.clear();
    await call('Shortcut.unregister_all', empty);
  },
  async isRegistered(accelerator: string): Promise<boolean> {
    return (await call<{ value: boolean }>('Shortcut.is_registered', { accelerator })).value;
  },
  async list(): Promise<Array<{ id: number; accelerator: string }>> {
    return (await call<{ shortcuts: Array<{ id: number; accelerator: string }> }>('Shortcut.list', empty)).shortcuts;
  },
  onTriggered(listener: (e: ShortcutTriggeredEvent) => void): Unsubscribe {
    return boson.on<ShortcutTriggeredEvent>('shortcut:triggered', listener);
  },
};

export { BosonIpcError };
