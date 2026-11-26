// Typed helpers for the native controllers: Dialog, Permissions, Zoom and Native (metrics, locale, system
// preferences, session end, close-to-tray) plus Web Notification click events. See docs/NATIVE.md.
import { boson, type Unsubscribe } from './boson';
import type {
  AppMetrics,
  FileDialogReply,
  FileDialogRequest,
  LocaleInfo,
  MessageBoxReply,
  MessageBoxRequest,
  PermissionChangedEvent,
  PermissionPromptEvent,
  PermissionQueryReply,
  PermissionRule,
  SessionEndEvent,
  SessionQueryEndEvent,
  ShellResult,
  SystemPreferences,
  TrayHiddenEvent,
  WebNotificationEventPayload,
  ZoomState,
} from './generated/boson-bindings';

export type {
  AppMetrics,
  FileDialogReply,
  FileDialogRequest,
  LocaleInfo,
  MessageBoxReply,
  MessageBoxRequest,
  PermissionPromptEvent,
  PermissionRule,
  SystemPreferences,
  ZoomState,
};

// ---- Dialog ------------------------------------------------------------------------------------------------

export interface FileFilter {
  name: string;
  /** Bare extensions ("txt", "tar.gz") or "*" for all files. */
  extensions: string[];
}

export type MessageBoxType = 'none' | 'info' | 'warning' | 'error' | 'question' | 'shield';

/** Electron dialog.showOpenDialog / showSaveDialog options (the subset Windows supports). */
export interface ElectronFileDialogOptions {
  title?: string;
  defaultPath?: string;
  buttonLabel?: string;
  filters?: FileFilter[];
  properties?: Array<
    'openFile' | 'openDirectory' | 'multiSelections' | 'showHiddenFiles' | 'dontAddToRecent' | 'showOverwriteConfirmation' | 'createDirectory' | 'promptToCreate'
  >;
}

export interface ElectronMessageBoxOptions {
  message: string;
  type?: MessageBoxType;
  buttons?: string[];
  defaultId?: number;
  cancelId?: number;
  title?: string;
  detail?: string;
  checkboxLabel?: string;
  checkboxChecked?: boolean;
  noLink?: boolean;
}

function fileRequest(options: Partial<FileDialogRequest>): Partial<FileDialogRequest> {
  return { ...options };
}

function fromElectron(options: ElectronFileDialogOptions): Partial<FileDialogRequest> {
  const props = options.properties ?? [];
  return {
    title: options.title ?? '',
    default_path: options.defaultPath ?? '',
    button_label: options.buttonLabel ?? '',
    filters: options.filters ?? [],
    multiple: props.includes('multiSelections'),
    show_hidden: props.includes('showHiddenFiles'),
    add_to_recent: !props.includes('dontAddToRecent'),
  };
}

export const dialog = {
  open: (options: Partial<FileDialogRequest> = {}) => boson.call<FileDialogReply, Partial<FileDialogRequest>>('Dialog.open', fileRequest(options)),
  save: (options: Partial<FileDialogRequest> = {}) => boson.call<FileDialogReply, Partial<FileDialogRequest>>('Dialog.save', fileRequest(options)),
  pickFolder: (options: Partial<FileDialogRequest> = {}) =>
    boson.call<FileDialogReply, Partial<FileDialogRequest>>('Dialog.pick_folder', fileRequest(options)),
  messageBox: (options: Partial<MessageBoxRequest>) => boson.call<MessageBoxReply, Partial<MessageBoxRequest>>('Dialog.message_box', options),
  errorBox: (title: string, content: string) => boson.call<void, { title: string; content: string }>('Dialog.error_box', { title, content }),

  /** Electron-shaped: resolves { canceled, filePaths }. openDirectory picks folders. */
  async showOpenDialog(options: ElectronFileDialogOptions = {}): Promise<{ canceled: boolean; filePaths: string[] }> {
    const request = fromElectron(options);
    const folder = (options.properties ?? []).includes('openDirectory');
    const reply = folder ? await dialog.pickFolder(request) : await dialog.open(request);
    return { canceled: reply.cancelled, filePaths: reply.paths };
  },
  /** Electron-shaped: resolves { canceled, filePath }. */
  async showSaveDialog(options: ElectronFileDialogOptions = {}): Promise<{ canceled: boolean; filePath: string }> {
    const request = fromElectron(options);
    request.confirm_overwrite = true;
    const reply = await dialog.save(request);
    return { canceled: reply.cancelled, filePath: reply.path };
  },
  /** Electron-shaped: resolves { response, checkboxChecked }. */
  async showMessageBox(options: ElectronMessageBoxOptions): Promise<{ response: number; checkboxChecked: boolean }> {
    const reply = await dialog.messageBox({
      message: options.message,
      type: options.type ?? 'none',
      buttons: options.buttons ?? [],
      default_id: options.defaultId ?? 0,
      cancel_id: options.cancelId ?? -1,
      title: options.title ?? '',
      detail: options.detail ?? '',
      checkbox_label: options.checkboxLabel ?? '',
      checkbox_checked: options.checkboxChecked ?? false,
      command_links: options.noLink === false,
    });
    return { response: reply.response, checkboxChecked: reply.checkbox_checked };
  },
  showErrorBox: (title: string, content: string) => dialog.errorBox(title, content),
};

// ---- Permissions -------------------------------------------------------------------------------------------

export type PermissionKind =
  | 'microphone'
  | 'camera'
  | 'geolocation'
  | 'notifications'
  | 'other_sensors'
  | 'clipboard_read'
  | 'multiple_automatic_downloads'
  | 'file_read_write'
  | 'autoplay'
  | 'local_fonts'
  | 'midi_sysex'
  | 'window_management'
  | 'persistent_storage';

export type PermissionDecision = 'allow' | 'deny' | 'ask' | 'default';

export interface PermissionAnswer {
  decision: PermissionDecision;
  remember?: boolean;
}

export const permissions = {
  list: () => boson.call<PermissionRule[]>('Permissions.list'),
  query: (origin: string, kind: PermissionKind) =>
    boson.call<PermissionQueryReply, { origin: string; kind: string }>('Permissions.query', { origin, kind }),
  set: (origin: string, kind: PermissionKind, decision: PermissionDecision) =>
    boson.call<ShellResult, { origin: string; kind: string; decision: string }>('Permissions.set', { origin, kind, decision }),
  reset: (origin: string, kind: PermissionKind) => boson.call<ShellResult, { origin: string; kind: string }>('Permissions.reset', { origin, kind }),
  /** Empty origin forgets every rule. */
  clear: (origin = '') => boson.call<ShellResult, { origin: string }>('Permissions.clear', { origin }),
  pending: () => boson.call<PermissionPromptEvent[]>('Permissions.pending'),
  respond: (id: number, decision: PermissionDecision, remember = false) =>
    boson.call<ShellResult, { id: number; decision: string; remember: boolean }>('Permissions.respond', { id, decision, remember }),

  /**
   * Installs the JS prompt hook: every request the policy resolves to "ask" calls handler, and its answer
   * completes the WebView2 request. A throwing / rejecting handler answers "default" (WebView2's own UI).
   * The returned function removes the hook (pending prompts then fall back to "default").
   */
  onPrompt(handler: (prompt: PermissionPromptEvent) => PermissionAnswer | Promise<PermissionAnswer>): Unsubscribe {
    const off = boson.on<PermissionPromptEvent>('permission:prompt', (prompt) => {
      Promise.resolve()
        .then(() => handler(prompt))
        .then(
          (answer) => permissions.respond(prompt.id, answer.decision, answer.remember ?? false),
          () => permissions.respond(prompt.id, 'default', false)
        )
        .catch((err: unknown) => console.error('[boson/native] permission respond failed', err));
    });
    void boson.call('Permissions.set_prompt_handler', { enabled: true }).catch(() => {});
    return () => {
      off();
      void boson.call('Permissions.set_prompt_handler', { enabled: false }).catch(() => {});
    };
  },
  onChanged: (listener: (rules: PermissionRule[]) => void): Unsubscribe =>
    boson.on<PermissionChangedEvent>('permission:changed', (e) => listener(e.rules)),
};

// ---- Zoom --------------------------------------------------------------------------------------------------

export const zoom = {
  get: () => boson.call<ZoomState>('Zoom.get'),
  set: (factor: number) => boson.call<ZoomState, { factor: number }>('Zoom.set', { factor }),
  /** Chromium / Electron zoom level: factor = 1.2 ^ level. */
  setLevel: (level: number) => boson.call<ZoomState, { level: number }>('Zoom.set_level', { level }),
  zoomIn: () => boson.call<ZoomState>('Zoom.zoom_in'),
  zoomOut: () => boson.call<ZoomState>('Zoom.zoom_out'),
  reset: () => boson.call<ZoomState>('Zoom.reset'),
  onChanged: (listener: (state: ZoomState) => void): Unsubscribe => boson.on<ZoomState>('zoom:changed', listener),
};

// ---- App: metrics, locale, preferences, session end, tray ---------------------------------------------------

/** Electron app.getAppMetrics() shape (memory in KiB, like Electron). */
export interface ElectronProcessMetric {
  pid: number;
  type: string;
  creationTime: number;
  cpu: { percentCPUUsage: number; cumulativeCPUUsage: number; idleWakeupsPerSecond: number };
  memory: { workingSetSize: number; peakWorkingSetSize: number; privateBytes: number };
}

export const app = {
  /** This process + every WebView2 process: memory and CPU since the previous call. */
  metrics: () => boson.call<AppMetrics>('Native.metrics'),
  async getAppMetrics(): Promise<ElectronProcessMetric[]> {
    const m = await app.metrics();
    return m.processes.map((p) => ({
      pid: p.pid,
      type: p.type === 'app' ? 'Browser' : p.type === 'renderer' ? 'Tab' : p.type === 'gpu' ? 'GPU' : 'Utility',
      creationTime: p.created_unix_ms,
      cpu: { percentCPUUsage: p.cpu_percent, cumulativeCPUUsage: p.cpu_time_ms / 1000, idleWakeupsPerSecond: 0 },
      memory: {
        workingSetSize: Math.round(p.working_set_bytes / 1024),
        peakWorkingSetSize: Math.round(p.peak_working_set_bytes / 1024),
        privateBytes: Math.round(p.private_bytes / 1024),
      },
    }));
  },
  locale: () => boson.call<LocaleInfo>('Native.locale'),
  async getLocale(): Promise<string> {
    return (await app.locale()).ui_language;
  },
  async getSystemLocale(): Promise<string> {
    return (await app.locale()).user_locale;
  },
  async getPreferredSystemLanguages(): Promise<string[]> {
    return (await app.locale()).preferred_languages;
  },
  systemPreferences: () => boson.call<SystemPreferences>('Native.system_preferences'),
  onSystemPreferencesChanged: (listener: (prefs: SystemPreferences) => void): Unsubscribe =>
    boson.on<SystemPreferences>('system:preferences-changed', listener),
  onLocaleChanged: (listener: (locale: LocaleInfo) => void): Unsubscribe => boson.on<LocaleInfo>('system:locale-changed', listener),

  /** Non-empty reason blocks logoff / shutdown (shown by Windows); empty clears it. */
  setShutdownBlockReason: (reason: string) => boson.call<ShellResult, { reason: string }>('Native.set_shutdown_block_reason', { reason }),
  onQueryEndSession: (listener: (e: SessionQueryEndEvent) => void): Unsubscribe => boson.on<SessionQueryEndEvent>('session:query-end', listener),
  /** ending = true: the process is about to be terminated; anything async may not finish. */
  onSessionEnd: (listener: (e: SessionEndEvent) => void): Unsubscribe => boson.on<SessionEndEvent>('session:end', listener),

  setCloseToTray: (enabled: boolean) => boson.call<ShellResult, { enabled: boolean }>('Native.set_close_to_tray_enabled', { enabled }),
  hideToTray: () => boson.call<ShellResult>('Native.hide_to_tray'),
  restoreFromTray: () => boson.call<ShellResult>('Native.restore_from_tray'),
  onHiddenToTray: (listener: (hidden: boolean) => void): Unsubscribe =>
    boson.on<TrayHiddenEvent>('tray:window-hidden', (e) => listener(e.hidden)),
};

/**
 * Mirrors SystemPreferences onto <html>: data-boson-reduced-motion, data-boson-high-contrast and the
 * --boson-text-scale / --boson-system-accent custom properties. Returns the unsubscribe function.
 */
export function applySystemPreferences(root: HTMLElement = document.documentElement): Unsubscribe {
  const apply = (p: SystemPreferences): void => {
    root.dataset.bosonReducedMotion = String(p.reduced_motion);
    root.dataset.bosonHighContrast = String(p.high_contrast);
    root.style.setProperty('--boson-text-scale', String(p.text_scale_factor));
    root.style.setProperty('--boson-system-accent', p.accent_color);
  };
  void app.systemPreferences().then(apply).catch(() => {});
  return app.onSystemPreferencesChanged(apply);
}

// ---- Web notifications (page uses the standard Notification API; these are app-shell events) -------------

export const webNotifications = {
  onShown: (listener: (n: WebNotificationEventPayload) => void): Unsubscribe => boson.on<WebNotificationEventPayload>('web-notification:shown', listener),
  onClicked: (listener: (n: WebNotificationEventPayload) => void): Unsubscribe =>
    boson.on<WebNotificationEventPayload>('web-notification:clicked', listener),
  onClosed: (listener: (n: WebNotificationEventPayload) => void): Unsubscribe => boson.on<WebNotificationEventPayload>('web-notification:closed', listener),
};
