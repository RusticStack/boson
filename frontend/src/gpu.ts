// Typed helpers for native GPU surfaces (C++ gpu::Surface via the "Gpu" controller) and the <boson-surface>
// element. The element runtime (window.__bosonGpu) is injected by the backend (GpuController::page_script() /
// GpuController::attach); these helpers work with or without it. See docs/GPU.md.
import { boson, BosonIpcError, type Unsubscribe } from './boson';
import type {
  GpuBoundsUpdate,
  GpuCreateSurfaceRequest,
  GpuDeviceLostEvent,
  GpuInfo,
  GpuOk,
  GpuRect,
  GpuSetBoundsRequest,
  GpuSurfaceInfo,
  GpuSurfaceRef,
} from './generated/boson-bindings';

export type { GpuBoundsUpdate, GpuDeviceLostEvent, GpuInfo, GpuOk, GpuRect, GpuSurfaceInfo };

export type GpuLayer = 'below' | 'above';

export interface CreateSurfaceOptions {
  /** Name the C++ app registered with GpuController::register_content. */
  content: string;
  /** Below (default) or above the WebView. Below needs a transparent page background where it should show. */
  layer?: GpuLayer;
  zIndex?: number;
  opaque?: boolean;
  vsync?: boolean;
}

/** window.__bosonGpu, installed by GpuController::page_script(). */
export interface BosonGpuRuntime {
  readonly version: number;
  /** Per-document token; surfaces created with an older token are destroyed by the backend. */
  readonly page: string;
  call<T = unknown>(method: string, params?: unknown): Promise<T>;
  /** Re-reports the bounds of every <boson-surface> on the next animation frame. */
  refresh(): void;
}

/** The <boson-surface content="..." layer="below|above" z-index="0" opaque vsync="false"> element. */
export interface BosonSurfaceElement extends HTMLElement {
  /** Native surface id (0 until 'boson-surface-ready'). */
  readonly surfaceId: number;
  requestFrame(): Promise<GpuOk>;
}

declare global {
  interface Window {
    __bosonGpu?: BosonGpuRuntime;
  }
  interface HTMLElementTagNameMap {
    'boson-surface': BosonSurfaceElement;
  }
  interface HTMLElementEventMap {
    'boson-surface-ready': CustomEvent<GpuSurfaceInfo>;
    'boson-surface-error': CustomEvent<string>;
  }
}

const documentToken = Math.random().toString(36).slice(2) + Date.now().toString(36);

function rectOf(element: Element): GpuRect {
  const r = element.getBoundingClientRect();
  return { x: r.left, y: r.top, width: r.width, height: r.height };
}

export const gpu = {
  /** Adapter, feature level, Vulkan availability, hosting mode and registered contents. */
  info(): Promise<GpuInfo> {
    return boson.call<GpuInfo>('Gpu.info');
  },

  /** Creates a native surface over rect (CSS px, viewport coordinates). Prefer <boson-surface> for DOM layout. */
  createSurface(options: CreateSurfaceOptions, rect: GpuRect | Element): Promise<GpuSurfaceInfo> {
    return boson.call<GpuSurfaceInfo, GpuCreateSurfaceRequest>('Gpu.create_surface', {
      content: options.content,
      layer: options.layer ?? 'below',
      z_index: options.zIndex ?? 0,
      rect: rect instanceof Element ? rectOf(rect) : rect,
      scale: window.devicePixelRatio,
      opaque: options.opaque ?? false,
      vsync: options.vsync ?? true,
      page: window.__bosonGpu?.page ?? documentToken,
    });
  },

  destroySurface(id: number): Promise<GpuOk> {
    return boson.call<GpuOk, GpuSurfaceRef>('Gpu.destroy', { id });
  },

  /** Moves / resizes / clips several surfaces at once (CSS px). Batch these per animation frame. */
  setBounds(surfaces: GpuBoundsUpdate[]): Promise<GpuOk> {
    return boson.call<GpuOk, GpuSetBoundsRequest>('Gpu.set_bounds', { scale: window.devicePixelRatio, surfaces });
  },

  requestFrame(id: number): Promise<GpuOk> {
    return boson.call<GpuOk, GpuSurfaceRef>('Gpu.request_frame', { id });
  },

  /** "gpu:device_lost": the device was removed / reset; surfaces are re-created when recovered is true. */
  onDeviceLost(listener: (event: GpuDeviceLostEvent) => void): Unsubscribe {
    return boson.on('gpu:device_lost', listener);
  },

  /** Resolves when the <boson-surface> runtime is installed (it is injected at document creation). */
  async whenReady(): Promise<BosonGpuRuntime> {
    if (!window.__bosonGpu && !boson.isAvailable()) {
      throw new BosonIpcError('WebView2 host bridge (window.chrome.webview) is unavailable.', -32001);
    }
    await customElements.whenDefined('boson-surface');
    const runtime = window.__bosonGpu;
    if (!runtime) {
      throw new BosonIpcError('Boson GPU runtime not installed (GpuController::page_script()).', -32002);
    }
    return runtime;
  },

  /**
   * Appends a <boson-surface> for content to parent (size it with style / CSS) and resolves once the backend
   * created the native surface.
   */
  mount(
    parent: HTMLElement,
    options: CreateSurfaceOptions,
    style?: Partial<CSSStyleDeclaration>,
  ): Promise<{ element: BosonSurfaceElement; info: GpuSurfaceInfo }> {
    const element = document.createElement('boson-surface');
    element.setAttribute('content', options.content);
    element.setAttribute('layer', options.layer ?? 'below');
    if (options.zIndex !== undefined) element.setAttribute('z-index', String(options.zIndex));
    if (options.opaque) element.setAttribute('opaque', '');
    if (options.vsync === false) element.setAttribute('vsync', 'false');
    if (style) Object.assign(element.style, style);
    return new Promise((resolve, reject) => {
      element.addEventListener('boson-surface-ready', (e) => resolve({ element, info: e.detail }), { once: true });
      element.addEventListener('boson-surface-error', (e) => reject(new BosonIpcError(e.detail)), { once: true });
      parent.appendChild(element);
    });
  },
};

export default gpu;
