/**
 * Native drag-out of files (Electron's webContents.startDrag) on top of the C++ "Drag" controller
 * (boson::ipc::DragController + boson::transfer::DragOut).
 *
 * The drag must start while a mouse button is down: call startDrag() from a dragstart / mousedown handler.
 * enableDragOut() wires that up (dragstart + preventDefault, so no HTML drag starts). The native side runs the
 * modal OLE drag loop on the UI thread; the promise settles when the drop / cancel happens.
 */
import { boson, BosonIpcError, type Unsubscribe } from './boson';

export type DragEffectName = 'copy' | 'move' | 'link';

export interface StartDragOptions {
  /** Drag image file (PNG / ICO / BMP / JPEG). Default: the shell's image for the items. */
  icon?: string;
  /** Effects the drop target may choose. Default: copy + link (never deletes the source). */
  effects?: DragEffectName[];
}

export interface DragResult {
  dragId: number;
  /** false: rejected before the drag loop (no button down, another drag running...). */
  started: boolean;
  effect: DragEffectName | 'none';
  error: string;
}

interface DragEndedPayload {
  drag_id: number;
  started: boolean;
  effect: DragEffectName | 'none';
  error: string;
}

const finished = new Map<number, DragEndedPayload>();
const waiters = new Map<number, (payload: DragEndedPayload) => void>();
let listening = false;

function ensureListener(): void {
  if (listening) {
    return;
  }
  listening = true;
  // "drag:ended" can arrive before the Drag.start response, so results are parked until claimed.
  boson.on<DragEndedPayload>('drag:ended', (payload) => {
    const waiter = waiters.get(payload.drag_id);
    if (waiter) {
      waiters.delete(payload.drag_id);
      waiter(payload);
    } else {
      finished.set(payload.drag_id, payload);
    }
  });
}

/** Starts a native file drag. Resolves when it ends; rejects with BosonIpcError when it was not started. */
export async function startDrag(files: string | string[], options: StartDragOptions = {}): Promise<DragResult> {
  ensureListener();
  const list = Array.isArray(files) ? files : [files];
  const { drag_id: dragId } = await boson.call<{ drag_id: number }, { files: string[]; icon: string; effects: string[] }>(
    'Drag.start',
    { files: list, icon: options.icon ?? '', effects: options.effects ?? [] }
  );
  const payload =
    finished.get(dragId) ??
    (await new Promise<DragEndedPayload>((resolve) => {
      waiters.set(dragId, resolve);
    }));
  finished.delete(dragId);
  if (!payload.started) {
    throw new BosonIpcError(`startDrag: ${payload.error}`, -32000);
  }
  return { dragId, started: payload.started, effect: payload.effect, error: payload.error };
}

/**
 * Makes element draggable and starts a native file drag on dragstart. getFiles returns the absolute paths
 * (or null to let the browser handle the drag). Returns the cleanup function.
 */
export function enableDragOut(
  element: HTMLElement,
  getFiles: (event: DragEvent) => string | string[] | null,
  options: StartDragOptions | ((event: DragEvent) => StartDragOptions) = {}
): Unsubscribe {
  const previous = element.draggable;
  element.draggable = true;
  const onDragStart = (event: DragEvent): void => {
    const files = getFiles(event);
    if (files === null || (Array.isArray(files) && files.length === 0)) {
      return;
    }
    event.preventDefault();
    const resolved = typeof options === 'function' ? options(event) : options;
    startDrag(files, resolved).catch((err: unknown) => {
      console.error('[boson] startDrag failed', err);
    });
  };
  element.addEventListener('dragstart', onDragStart);
  return () => {
    element.removeEventListener('dragstart', onDragStart);
    element.draggable = previous;
  };
}
