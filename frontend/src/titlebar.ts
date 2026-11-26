import { boson, type Unsubscribe } from './boson';

/** A rect in CSS px (DIPs at zoom 1). */
export interface TitlebarRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Payload of "titlebar:overlay" and of Titlebar.get_overlay / set_overlay. */
export interface TitlebarOverlayInfo {
  visible: boolean;
  /** Free titlebar area beside the caption buttons: the env(titlebar-area-*) equivalent. */
  titlebar_area: TitlebarRect;
  /** Rect occupied by the native caption buttons. */
  buttons: TitlebarRect;
  dpi: number;
  rtl: boolean;
  maximized: boolean;
  active: boolean;
}

export interface TitlebarOverlayOptions {
  /** CSS colour behind the buttons ("#rrggbb[aa]", "rgb()/rgba()", "transparent"). Default transparent. */
  color?: string;
  /** Glyph colour. Default: Windows 11 text colour of the theme. */
  symbol_color?: string;
  /** Minimize/maximize hover fill. Default: derived from symbol_color. */
  hover_color?: string;
  /** DIPs, default 32. */
  height?: number;
  /** DIPs per button, default 46. */
  button_width?: number;
  theme?: 'system' | 'light' | 'dark';
  /** Default: follows the window's RTL layout. */
  rtl?: boolean;
}

export interface SystemColors {
  window: string;
  window_text: string;
  highlight: string;
  highlight_text: string;
  button_face: string;
  button_text: string;
  gray_text: string;
  hotlight: string;
}

/** Payload of "theme:changed" and of Titlebar.get_theme. */
export interface ThemeInfo {
  dark_mode: boolean;
  system_dark_mode: boolean;
  high_contrast: boolean;
  high_contrast_scheme: string;
  transparency_effects: boolean;
  accent_on_title_bars: boolean;
  accent: string;
  accent_light: string[];
  accent_dark: string[];
  colors: SystemColors;
}

export function setTitlebarOverlay(options: TitlebarOverlayOptions | false): Promise<TitlebarOverlayInfo> {
  const params = options === false ? { enabled: false } : { enabled: true, ...options };
  return boson.call<TitlebarOverlayInfo, typeof params>('Titlebar.set_overlay', params);
}

export function getTitlebarOverlay(): Promise<TitlebarOverlayInfo> {
  return boson.call<TitlebarOverlayInfo>('Titlebar.get_overlay');
}

export function getTheme(): Promise<ThemeInfo> {
  return boson.call<ThemeInfo>('Titlebar.get_theme');
}

/** Shows the native window menu at a point in CSS px of this page, or at the cursor. */
export function showSystemMenu(x?: number, y?: number): Promise<void> {
  const atCursor = x === undefined || y === undefined;
  return boson.call<void, { x: number; y: number; at_cursor: boolean }>('Titlebar.show_system_menu', {
    x: x ?? 0,
    y: y ?? 0,
    at_cursor: atCursor,
  });
}

/**
 * Writes the overlay geometry into CSS custom properties on <html>:
 * --boson-titlebar-area-x / -y / -width / -height and --boson-caption-buttons-width / -height (px),
 * plus data-boson-overlay / data-boson-maximized / data-boson-active attributes.
 */
export function applyTitlebarOverlayCss(info: TitlebarOverlayInfo, root: HTMLElement = document.documentElement): void {
  const px = (v: number): string => String(Math.round(v * 100) / 100) + 'px';
  root.style.setProperty('--boson-titlebar-area-x', px(info.titlebar_area.x));
  root.style.setProperty('--boson-titlebar-area-y', px(info.titlebar_area.y));
  root.style.setProperty('--boson-titlebar-area-width', px(info.titlebar_area.width));
  root.style.setProperty('--boson-titlebar-area-height', px(info.titlebar_area.height));
  root.style.setProperty('--boson-caption-buttons-width', px(info.buttons.width));
  root.style.setProperty('--boson-caption-buttons-height', px(info.buttons.height));
  root.dataset.bosonOverlay = info.visible ? 'visible' : 'hidden';
  root.dataset.bosonMaximized = String(info.maximized);
  root.dataset.bosonActive = String(info.active);
}

/** Keeps the CSS properties in sync with "titlebar:overlay". Returns the unsubscribe function. */
export function watchTitlebarOverlay(listener?: (info: TitlebarOverlayInfo) => void): Unsubscribe {
  const handle = (info: TitlebarOverlayInfo): void => {
    applyTitlebarOverlayCss(info);
    listener?.(info);
  };
  const off = boson.on<TitlebarOverlayInfo>('titlebar:overlay', handle);
  if (boson.isAvailable()) {
    void getTitlebarOverlay().then(handle).catch(() => {});
  }
  return off;
}

/**
 * Writes the OS theme into <html>: data-boson-theme="dark|light", data-boson-high-contrast, and
 * --boson-accent, --boson-accent-light-1..3, --boson-accent-dark-1..3, --boson-sys-<name> colours.
 */
export function applyThemeCss(theme: ThemeInfo, root: HTMLElement = document.documentElement): void {
  root.dataset.bosonTheme = theme.dark_mode ? 'dark' : 'light';
  root.dataset.bosonHighContrast = String(theme.high_contrast);
  root.style.setProperty('--boson-accent', theme.accent);
  theme.accent_light.forEach((c, i) => root.style.setProperty('--boson-accent-light-' + String(i + 1), c));
  theme.accent_dark.forEach((c, i) => root.style.setProperty('--boson-accent-dark-' + String(i + 1), c));
  for (const [name, value] of Object.entries(theme.colors)) {
    root.style.setProperty('--boson-sys-' + name.replace(/_/g, '-'), value);
  }
}

/** Keeps the theme attributes / properties in sync with "theme:changed". */
export function watchTheme(listener?: (theme: ThemeInfo) => void): Unsubscribe {
  const handle = (theme: ThemeInfo): void => {
    applyThemeCss(theme);
    listener?.(theme);
  };
  const off = boson.on<ThemeInfo>('theme:changed', handle);
  if (boson.isAvailable()) {
    void getTheme().then(handle).catch(() => {});
  }
  return off;
}

function regionOf(element: Element | null): 'drag' | 'no-drag' | null {
  for (let el = element; el; el = el.parentElement) {
    const style = getComputedStyle(el);
    const value = (style.getPropertyValue('app-region') || style.getPropertyValue('-webkit-app-region')).trim();
    if (value === 'drag' || value === 'no-drag') {
      return value;
    }
  }
  return null;
}

/**
 * Fallback for runtimes that deliver right-clicks on CSS drag regions to the page: shows the native
 * window menu instead of the page context menu. (Composition hosting and titlebar_height areas are
 * handled natively by Boson's Titlebar without this.)
 */
export function enableDragRegionSystemMenu(): Unsubscribe {
  const onContextMenu = (event: MouseEvent): void => {
    if (regionOf(event.target instanceof Element ? event.target : null) === 'drag') {
      event.preventDefault();
      void showSystemMenu(event.clientX, event.clientY).catch(() => {});
    }
  };
  document.addEventListener('contextmenu', onContextMenu, true);
  return () => document.removeEventListener('contextmenu', onContextMenu, true);
}
