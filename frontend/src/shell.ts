// Typed helpers for the shell-integration controllers (Taskbar, Notification, Power, AutoLaunch).
// Only depends on './boson'; the generated bindings carry the same shapes once tsgen registers the controllers.
import { boson, type Unsubscribe } from './boson';

export interface ShellResult {
  ok: boolean;
  error: string;
}

/** First non-empty source wins: badge text, stock name, Segoe Fluent glyph codepoint, or icon file. */
export interface IconSpec {
  badge?: string;
  stock?: string;
  glyph?: number;
  file?: string;
  background?: string;
  foreground?: string;
  size?: number;
}

export type ProgressState = 'none' | 'indeterminate' | 'normal' | 'error' | 'paused';

export interface ThumbnailButtonSpec {
  id: number;
  icon?: IconSpec;
  tooltip?: string;
  enabled?: boolean;
  hidden?: boolean;
  dismiss_on_click?: boolean;
  no_background?: boolean;
}

export interface JumpListItemSpec {
  title?: string;
  description?: string;
  arguments?: string;
  icon_path?: string;
  icon_index?: number;
  separator?: boolean;
}

export interface JumpListResponse {
  ok: boolean;
  max_slots: number;
  skipped_removed: number;
  rejected_categories: string[];
  error: string;
}

export const taskbar = {
  setProgress: (state: ProgressState, value?: number) =>
    boson.call<ShellResult, { state: ProgressState; value?: number }>('Taskbar.set_progress', value === undefined ? { state } : { state, value }),
  clearProgress: () => boson.call<ShellResult>('Taskbar.clear_progress'),
  setOverlay: (icon: IconSpec, description = '') =>
    boson.call<ShellResult, { icon: IconSpec; description: string }>('Taskbar.set_overlay', { icon, description }),
  clearOverlay: () => boson.call<ShellResult>('Taskbar.clear_overlay'),
  flash: (opts: { count?: number; until_foreground?: boolean; caption?: boolean; tray?: boolean; interval_ms?: number } = {}) =>
    boson.call<ShellResult, typeof opts>('Taskbar.flash', opts),
  stopFlash: () => boson.call<ShellResult>('Taskbar.stop_flash'),
  setThumbnailButtons: (buttons: ThumbnailButtonSpec[]) =>
    boson.call<ShellResult, { buttons: ThumbnailButtonSpec[] }>('Taskbar.set_thumbnail_buttons', { buttons }),
  setThumbnailTooltip: (tooltip: string) => boson.call<ShellResult, { tooltip: string }>('Taskbar.set_thumbnail_tooltip', { tooltip }),
  /** DIPs in the window client area; pass null to show the whole window. */
  setThumbnailClip: (rect: { x: number; y: number; width: number; height: number } | null) =>
    boson.call<ShellResult, { x: number; y: number; width: number; height: number }>(
      'Taskbar.set_thumbnail_clip',
      rect ?? { x: 0, y: 0, width: 0, height: 0 }
    ),
  setJumpList: (list: {
    tasks?: JumpListItemSpec[];
    categories?: { name: string; items: JumpListItemSpec[] }[];
    show_recent?: boolean;
    show_frequent?: boolean;
  }) => boson.call<JumpListResponse, typeof list>('Taskbar.set_jump_list', list),
  clearJumpList: () => boson.call<ShellResult>('Taskbar.clear_jump_list'),
  addRecentDocument: (path: string) => boson.call<ShellResult, { path: string }>('Taskbar.add_recent_document', { path }),
  clearRecentDocuments: () => boson.call<ShellResult>('Taskbar.clear_recent_documents'),
  onThumbnailClick: (listener: (id: number) => void): Unsubscribe =>
    boson.on<{ id: number }>('taskbar:thumbnail-click', (e) => listener(e.id)),
};

export interface ToastProgressSpec {
  title?: string;
  value?: number | null;
  value_label?: string;
  status?: string;
}

export interface ToastRequest {
  title?: string;
  body?: string;
  attribution?: string;
  app_logo?: string;
  app_logo_circle?: boolean;
  hero_image?: string;
  inline_image?: string;
  launch_arguments?: string;
  buttons?: {
    content: string;
    arguments: string;
    activation?: 'foreground' | 'background' | 'protocol';
    input_id?: string;
    image?: string;
    context_menu?: boolean;
  }[];
  inputs?: { id: string; title?: string; placeholder?: string; default_value?: string; options?: { id: string; content: string }[] }[];
  progress?: ToastProgressSpec;
  scenario?: 'default' | 'reminder' | 'alarm' | 'incoming_call' | 'urgent';
  duration?: 'default' | 'short' | 'long';
  silent?: boolean;
  sound?: string;
  loop_sound?: boolean;
  tag?: string;
  group?: string;
  suppress_popup?: boolean;
  expires_after_seconds?: number;
  fallback?: boolean;
  level?: 'info' | 'warning' | 'error' | 'none';
}

export interface ToastResponse {
  ok: boolean;
  used_fallback: boolean;
  tag: string;
  group: string;
  error: string;
}

export interface ToastActivatedEvent {
  tag: string;
  group: string;
  arguments: string;
  user_input: Record<string, string>;
  source: 'toast' | 'activator';
}

export const notifications = {
  show: (req: ToastRequest) => boson.call<ToastResponse, ToastRequest>('Notification.show', req),
  updateProgress: (tag: string, progress: ToastProgressSpec, group = '') =>
    boson.call<ShellResult, { tag: string; group: string; progress: ToastProgressSpec }>('Notification.update_progress', { tag, group, progress }),
  hide: (tag: string, group = '') => boson.call<ShellResult, { tag: string; group: string }>('Notification.hide', { tag, group }),
  remove: (tag: string, group = '') => boson.call<ShellResult, { tag: string; group: string }>('Notification.remove', { tag, group }),
  clearAll: () => boson.call<ShellResult>('Notification.clear_all'),
  support: () => boson.call<{ supported: boolean; app_user_model_id: string; setting: string }>('Notification.support'),
  onActivated: (listener: (e: ToastActivatedEvent) => void): Unsubscribe => boson.on<ToastActivatedEvent>('notification:activated', listener),
  onDismissed: (listener: (e: { tag: string; group: string; reason: string }) => void): Unsubscribe =>
    boson.on('notification:dismissed', listener),
  onFailed: (listener: (e: { tag: string; group: string; error: number }) => void): Unsubscribe => boson.on('notification:failed', listener),
};

export interface PowerStatus {
  on_ac: boolean;
  has_battery: boolean;
  battery_percent?: number | null;
  charging: boolean;
  battery_saver: boolean;
  battery_seconds_remaining?: number | null;
  display: 'on' | 'off' | 'dimmed';
  session_locked: boolean;
  idle_ms: number;
}

export type PowerEventName =
  | 'suspend'
  | 'resume'
  | 'source-changed'
  | 'battery-changed'
  | 'battery-saver-changed'
  | 'display-changed'
  | 'lock'
  | 'unlock';

export const power = {
  status: () => boson.call<PowerStatus>('Power.status'),
  idleTime: () => boson.call<{ idle_ms: number }>('Power.idle_time'),
  idleState: (threshold_seconds: number) =>
    boson.call<{ state: 'active' | 'idle' | 'locked'; idle_ms: number }, { threshold_seconds: number }>('Power.idle_state', { threshold_seconds }),
  startBlocker: (type: 'prevent_app_suspension' | 'prevent_display_sleep', reason = '') =>
    boson.call<{ id: number; started: boolean }, { type: string; reason: string }>('Power.start_blocker', { type, reason }),
  stopBlocker: (id: number) => boson.call<ShellResult, { id: number }>('Power.stop_blocker', { id }),
  isBlockerStarted: (id: number) => boson.call<{ id: number; started: boolean }, { id: number }>('Power.is_blocker_started', { id }),
  on: (name: PowerEventName, listener: (e: { kind: PowerEventName; status: PowerStatus }) => void): Unsubscribe =>
    boson.on(`power:${name}`, listener),
};

export interface AutoLaunchStatus {
  registered: boolean;
  enabled: boolean;
  disabled_by_user: boolean;
  executable: string;
  args: string[];
}

export const autoLaunch = {
  status: () => boson.call<AutoLaunchStatus>('AutoLaunch.status'),
  enable: (args: string[] = []) => boson.call<AutoLaunchStatus, { args: string[] }>('AutoLaunch.enable', { args }),
  disable: () => boson.call<AutoLaunchStatus>('AutoLaunch.disable'),
};
