import { boson, type Unsubscribe } from './boson';

/** One node of a native menu (Boson `Menu.*` controller). Only `label` (or `role`, or `type: 'separator'`) is required. */
export interface MenuItemTemplate {
  id?: string;
  /** Win32 mnemonic syntax: "&File" underlines F; "&&" is a literal ampersand. */
  label?: string;
  /** "Ctrl+Shift+P", "CmdOrCtrl+S", "F11", "Alt+Enter" ... Shown in the menu; bound as a shortcut in the application menu. */
  accelerator?: string;
  type?: 'normal' | 'separator' | 'checkbox' | 'radio' | 'submenu';
  /** undo, redo, cut, copy, paste, pasteAndMatchStyle, delete, selectAll, reload, forceReload, toggleDevTools,
   * resetZoom, zoomIn, zoomOut, toggleFullscreen, minimize, maximize, close, showWindow, hideWindow, quit. */
  role?: string;
  enabled?: boolean;
  checked?: boolean;
  visible?: boolean;
  /** Radio group name (exclusive within the same submenu). */
  group?: string;
  submenu?: MenuItemTemplate[];
}

export interface MenuPopupResult {
  selected: boolean;
  id: string;
  checked: boolean;
}

export interface MenuBarEntryInfo {
  index: number;
  id: string;
  label: string;
  text: string;
  mnemonic: string;
  enabled: boolean;
  has_submenu: boolean;
}

export interface MenuBarInfo {
  mode: 'native' | 'popups' | string;
  entries: MenuBarEntryInfo[];
}

export interface MenuItemUpdate {
  id: string;
  target?: 'application' | 'tray';
  enabled?: boolean;
  checked?: boolean;
  visible?: boolean;
  label?: string;
  accelerator?: string;
}

export interface MenuClickEvent {
  id: string;
  source: 'application' | 'context' | 'tray' | 'accelerator' | string;
  checked: boolean;
}

export interface TrayClickEvent {
  button: 'left' | 'right' | 'double' | 'middle' | 'keyboard' | 'balloon-click' | 'balloon-dismissed' | string;
  x: number;
  y: number;
}

export interface TrayOptions {
  tooltip?: string;
  /** PNG / ICO bytes as base64 or a data: URL. */
  icon?: string;
  menu?: MenuItemTemplate[];
  menuOnLeftClick?: boolean;
}

interface BoolResponse {
  value: boolean;
}

interface CssRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

const call = <T>(method: string, params?: object): Promise<T> => boson.call<T, object>(method, params);

const rectOf = (element: Element): CssRect => {
  const r = element.getBoundingClientRect();
  return { x: r.left, y: r.top, width: r.width, height: r.height };
};

/** Replaces the window's application menu (native menu bar, or popups for a headless title bar). */
export async function setApplicationMenu(items: MenuItemTemplate[]): Promise<boolean> {
  return (await call<BoolResponse>('Menu.set_application_menu', { items })).value;
}

export function clearApplicationMenu(): Promise<void> {
  return call<void>('Menu.clear_application_menu');
}

export function getApplicationMenu(): Promise<MenuBarInfo> {
  return call<MenuBarInfo>('Menu.get_application_menu');
}

/** Opens one top-level submenu of the application menu below `anchor` (an HTML title bar button). */
export function popupApplicationMenu(entry: string | number, anchor?: Element): Promise<MenuPopupResult> {
  return call<MenuPopupResult>('Menu.popup_application_menu', {
    id: typeof entry === 'string' ? entry : '',
    index: typeof entry === 'number' ? entry : -1,
    rect: anchor ? rectOf(anchor) : null,
  });
}

/**
 * Shows a native context menu. `at` is a point in CSS pixels of this page (a MouseEvent works); omitted = cursor.
 * Resolves with the chosen item (`selected: false` when dismissed); clicks also fire "menu:click".
 */
export function popupContextMenu(items: MenuItemTemplate[], at?: { x: number; y: number } | MouseEvent): Promise<MenuPopupResult> {
  const point = at instanceof MouseEvent ? { x: at.clientX, y: at.clientY } : at;
  return call<MenuPopupResult>('Menu.popup_context_menu', {
    items,
    x: point ? point.x : null,
    y: point ? point.y : null,
  });
}

/** Replaces the browser context menu of `target` with a native one built by `items` on each right click. */
export function bindContextMenu(
  target: HTMLElement | Document,
  items: (event: MouseEvent) => MenuItemTemplate[] | null,
): Unsubscribe {
  const handler = (event: Event): void => {
    const mouse = event as MouseEvent;
    const template = items(mouse);
    if (!template || template.length === 0) {
      return;
    }
    event.preventDefault();
    void popupContextMenu(template, mouse).catch(() => {});
  };
  target.addEventListener('contextmenu', handler);
  return () => target.removeEventListener('contextmenu', handler);
}

export async function updateMenuItem(update: MenuItemUpdate): Promise<boolean> {
  return (await call<BoolResponse>('Menu.update_menu_item', { target: 'application', ...update })).value;
}

export function onMenuClick(listener: (event: MenuClickEvent) => void): Unsubscribe {
  return boson.on('menu:click', (data) => listener(data as MenuClickEvent));
}

export async function showTray(options: TrayOptions = {}): Promise<boolean> {
  const response = await call<BoolResponse>('Menu.tray_show', {
    tooltip: options.tooltip ?? '',
    icon_base64: options.icon ?? '',
    menu: options.menu ?? [],
    menu_on_left_click: options.menuOnLeftClick ?? false,
  });
  return response.value;
}

export function hideTray(): Promise<void> {
  return call<void>('Menu.tray_hide');
}

export async function setTrayMenu(items: MenuItemTemplate[]): Promise<boolean> {
  return (await call<BoolResponse>('Menu.tray_set_menu', { items })).value;
}

export async function setTrayTooltip(tooltip: string): Promise<boolean> {
  return (await call<BoolResponse>('Menu.tray_set_tooltip', { tooltip })).value;
}

export async function setTrayIcon(iconBase64: string): Promise<boolean> {
  return (await call<BoolResponse>('Menu.tray_set_icon', { icon_base64: iconBase64 })).value;
}

export async function showTrayBalloon(
  title: string,
  body: string,
  level: 'info' | 'warning' | 'error' | 'none' = 'info',
  silent = false,
): Promise<boolean> {
  return (await call<BoolResponse>('Menu.tray_show_balloon', { title, body, level, silent })).value;
}

export function onTrayClick(listener: (event: TrayClickEvent) => void): Unsubscribe {
  return boson.on('tray:click', (data) => listener(data as TrayClickEvent));
}

export interface TitlebarMenuOptions {
  /** CSS class of each generated button (default "boson-menubar-item"). */
  buttonClass?: string;
  /** Called with the button and its entry after it is created (for icons, extra classes...). */
  decorate?: (button: HTMLButtonElement, entry: MenuBarEntryInfo) => void;
}

/**
 * Renders the application menu's top-level entries as buttons inside `container` (an HTML title bar) and drops
 * the native submenus from them, VS Code style: hovering another button while a menu is open switches menus,
 * Left/Right arrows move between them, and Alt+mnemonic / F10 open them while the page has focus.
 * Call it after setApplicationMenu(). Returns a cleanup function.
 */
export async function attachTitlebarMenu(container: HTMLElement, options: TitlebarMenuOptions = {}): Promise<Unsubscribe> {
  const info = await getApplicationMenu();
  const buttons: HTMLButtonElement[] = [];
  const cleanups: Array<() => void> = [];
  container.replaceChildren();
  container.setAttribute('role', 'menubar');

  let open = false;
  for (const entry of info.entries) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = options.buttonClass ?? 'boson-menubar-item';
    button.setAttribute('role', 'menuitem');
    button.setAttribute('aria-haspopup', entry.has_submenu ? 'menu' : 'false');
    button.disabled = !entry.enabled;
    button.textContent = entry.text;
    button.style.setProperty('app-region', 'no-drag');
    button.style.setProperty('-webkit-app-region', 'no-drag');
    const onDown = (event: MouseEvent): void => {
      if (event.button !== 0 || open) {
        return;
      }
      event.preventDefault();
      open = true;
      button.setAttribute('aria-expanded', 'true');
      void popupApplicationMenu(entry.index, button)
        .catch(() => undefined)
        .finally(() => {
          open = false;
          button.removeAttribute('aria-expanded');
        });
    };
    button.addEventListener('mousedown', onDown);
    cleanups.push(() => button.removeEventListener('mousedown', onDown));
    options.decorate?.(button, entry);
    container.appendChild(button);
    buttons.push(button);
  }

  let frame = 0;
  const sendAnchors = (): void => {
    frame = 0;
    void call<void>('Menu.set_application_menu_anchors', { rects: buttons.map(rectOf) }).catch(() => {});
  };
  const schedule = (): void => {
    if (frame === 0) {
      frame = requestAnimationFrame(sendAnchors);
    }
  };
  const observer = new ResizeObserver(schedule);
  observer.observe(container);
  for (const button of buttons) {
    observer.observe(button);
  }
  window.addEventListener('resize', schedule);
  schedule();

  return () => {
    observer.disconnect();
    window.removeEventListener('resize', schedule);
    if (frame !== 0) {
      cancelAnimationFrame(frame);
    }
    for (const cleanup of cleanups) {
      cleanup();
    }
    container.replaceChildren();
    void call<void>('Menu.set_application_menu_anchors', { rects: [] }).catch(() => {});
  };
}
