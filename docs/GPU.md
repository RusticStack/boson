# Native GPU surfaces (`boson_gpu`)

Render native Direct3D (or Vulkan) content in the same window as the WebView, composited by DirectComposition below or above the page, positioned from HTML with a `<boson-surface>` element or from C++. Built for terminal grids, editors' minimaps, canvases, video/3D viewports: content that should not pay for DOM or WebGL.

`boson_gpu` is a separate static library (links `d3d11 d3d12 dxgi dwrite dcomp`); apps that do not use it never load those DLLs. Include `<boson/gpu/gpu.hpp>` and link `boson_gpu`. Vulkan is optional: `vulkan-1.dll` is loaded at runtime from System32 only when `VulkanInterop::create` is called.

## Architecture

| Piece | Header | What it does |
|---|---|---|
| `gpu::Device` | `gpu/device.hpp` | D3D11 or D3D12 (+ D3D11On12 for the built-in renderer) device. Adapter choice via `IDXGIFactory6::EnumAdapterByGpuPreference` (`Default`, `HighPerformance`, `MinimumPower`, `Warp`), explicit LUID, feature level 12_1 down to 10_0, tearing support, WARP fallback. Device-removed detection without polling (`ID3D11Device4::RegisterDeviceRemovedEvent`, D3D12 fence at `UINT64_MAX`), automatic recovery. |
| `gpu::RenderLoop` | `gpu/render_loop.hpp` | One `std::jthread` per device. Sleeps in `WaitForMultipleObjects` (wake event, device-removed event, swap-chain waitables of dirty surfaces) and, while frames are pending, `DCompositionWaitForCompositorClock` when available. No timers: idle = blocked, zero CPU. |
| `gpu::SurfaceHost` / `gpu::Surface` | `gpu/surface.hpp` | Flip-model composition swap chain (`CreateSwapChainForComposition`, FLIP_SEQUENTIAL, premultiplied alpha, `SetMaximumFrameLatency(1)` + waitable object, `ALLOW_TEARING` when supported) in a DirectComposition visual. Layer below or above the WebView, `z_index` within a layer, clip, opacity, visibility. |
| `gpu::Renderer2D` | `gpu/renderer2d.hpp` | Instanced 2D batcher: solid rects, SDF rounded rects (fill + stroke), textured quads, text from a DirectWrite glyph atlas, and a `TextGrid` fast path (one quad per non-empty background run + one per glyph). Two draw calls for a full terminal screen. |
| `gpu::GlyphAtlas` | `gpu/renderer2d.hpp` | DirectWrite `CreateGlyphRunAnalysis` (grayscale AA, `ALIASED_1x1` texture), shelf-packed R8 atlas, rasterized on first use. |
| `gpu::VulkanInterop` / `SharedImage` | `gpu/vulkan_interop.hpp` | Vulkan 1.2 device on the same adapter (matched by LUID). Images are D3D shared NT-handle textures (D3D11) or shared committed resources (D3D12) imported with `VK_KHR_external_memory_win32`; sync via a shared D3D fence imported as a Vulkan timeline semaphore (`VK_KHR_external_semaphore_win32`), CPU fallback when the import is not supported. |
| `ipc::GpuController` | `ipc/controllers/gpu_controller.hpp` | JS name `Gpu`: `create_surface`, `destroy`, `set_bounds`, `request_frame`, `info`; event `gpu:device_lost`. `page_script()` defines `<boson-surface>`. |

### Hosting

- **Composition hosting** (`HostingMode::Composition`): surfaces are children of `CompositionHost::underlay()` (below every WebView visual) or `overlay()` (above). One commit, one DComp tree.
- **Windowed hosting**: `SurfaceHost` creates its own DComp targets on the parent HWND: `topmost=FALSE` for the below layer (drawn under child windows, so under the WebView2 child HWND) and `topmost=TRUE` for the above layer.
- Content below the WebView shows where the page is transparent: set `WebViewConfig::transparent_background = true` and leave those page regions without background.

### Threading model

| Thread | Work |
|---|---|
| UI thread | DComp visual tree changes (`set_bounds`, `set_clip`, layer, commit). Coalesced: one `Commit()` per message-loop turn. Never waits on the GPU. |
| Render thread (one per `Device`) | Swap-chain creation/resize, frame callbacks, `Present`, device-lost recovery. Swap-chain content is attached to the visual by posting back to the UI thread. |
| IPC (`TaskRunner`) | `Gpu.*` calls arrive on the pool; `create_surface` runs on the UI thread through `UiMarshal::invoke_sync`, `set_bounds` is posted (`post_task`) so the pool never waits for the UI. |

The frame callback runs under `Device::lock()` (a recursive mutex) so the immediate context is never used from two threads; take the same lock for any other immediate-context work.

### Rendering model

- A surface presents only when dirty. `request_frame()` sets an atomic flag and wakes the loop only on the clean→dirty edge, so any number of requests before the next frame produce one frame (measured: 100 calls → 1 frame).
- Animated content calls `f.surface.request_frame()` inside its callback; frames are then paced by the swap-chain waitable (frame latency 1) and the compositor clock: at the display rate, never faster.
- Resize is lazy: `set_bounds` stores the size; `ResizeBuffers` runs on the render thread before the next frame.
- Minimize, `set_visible(false)` and occlusion (`DXGI_STATUS_OCCLUDED`, then `DXGI_PRESENT_TEST` probes every 250 ms only while occluded) pause rendering; restore repaints.
- DPI: `SurfaceHost` listens to `WM_DPICHANGED` and updates every surface's `dpi_scale` (in `FrameContext`); `<boson-surface>` reports `devicePixelRatio` changes via `matchMedia`.

### Device lost

On `DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG` (from `Present`, a callback HRESULT or the removed event), the render thread: notifies `DeviceResourceOwner`s to release everything (swap chains, renderers, atlases, Vulkan images); because a D3D12 device is a per-adapter singleton, all of that must happen before the new device can be created; then recreates the device on the same adapter (or the next per the preference), bumps `Device::generation()`, recreates swap chains and marks every surface dirty. If creation fails it retries every 50 ms. Listeners (`Device::on_device_lost`) and JS (`gpu:device_lost` with `reason`, `recovered`, `generation`) are notified. `Renderer2D` and `GlyphAtlas` recreate their resources lazily; a `VulkanInterop` must be recreated by the app (its Vulkan device belonged to the lost adapter state).

### Shader choice

Shaders are **precompiled at build time** with `fxc` from the Windows SDK (`vs_4_0` / `ps_4_0`, `/O3 /WX`) into headers (`/Fh /Vn`) under `build/generated/gpu`, and embedded as byte arrays. No runtime `d3dcompiler_47.dll`, no shader compile on first frame, no files on disk. Shader model 4.0 runs on every D3D feature level Boson accepts (10_0+) and on D3D11On12. DXC/SM6 is not needed: the batcher uses no wave ops or 16-bit types.

## C++ API

```cpp
#include <boson/gpu/gpu.hpp>

auto device = boson::gpu::Device::create({.backend = boson::gpu::Backend::D3D11,
                                          .adapter = boson::gpu::AdapterPreference::HighPerformance});
boson::gpu::SurfaceHost surfaces(webview_host);   // or SurfaceHost(window, composition_host)
auto surface = surfaces.create_surface(device, {.layer = boson::gpu::Layer::BelowWebView,
                                                .bounds = RECT{0, 0, 800, 600}});

auto renderer = std::make_shared<boson::gpu::Renderer2D>(device);
auto atlas = std::make_shared<boson::gpu::GlyphAtlas>(device, boson::gpu::FontSpec{.size_px = 15});
surface->set_frame_callback([renderer, atlas](boson::gpu::FrameContext& f) {
    renderer->begin(f, boson::gpu::Color{0.05f, 0.05f, 0.07f, 1});
    renderer->fill_rounded_rect({20, 20, 200, 80}, 12, boson::gpu::Color::rgba(0x4C6EF5FF));
    renderer->draw_text(*atlas, 32, 44, "hello from D3D", {1, 1, 1, 1});
    renderer->end();
    // f.surface.request_frame();  // only for animation
});
surface->request_frame();
surface->set_bounds(RECT{100, 100, 900, 700});    // physical px, client coordinates
```

Raw D3D: `FrameContext` exposes `back_buffer` / `render_target` (D3D11 or the D3D11On12 wrapper) and `d3d12_back_buffer` + `buffer_index` for D3D12 rendering; set `f.present = false` to skip the present.

Vulkan:

```cpp
std::string why;
auto vk = boson::gpu::VulkanInterop::create(device, &why);   // nullptr + reason when vulkan-1.dll / extensions missing
auto image = vk->create_image(width, height);
image->begin_vulkan();                   // records into image->command_buffer() inside image->render_pass()
image->clear_rects(bg, rects);           // or your own vkCmd* using framebuffer()/view()
image->end_vulkan();                     // submit + signal the shared fence
image->acquire_d3d();                    // D3D waits on the fence (GPU-side)
renderer.draw_texture(image->srv(), dst);
image->release_d3d();                    // signal back to Vulkan
```

Create the interop off the UI and render threads (first creation takes 50-450 ms): the demo uses `TaskRunner::spawn`.

## JavaScript

Register content in C++ and add the page script:

```cpp
config.transparent_background = true;
config.scripts_on_document_created.push_back(boson::ipc::GpuController::page_script());
boson::ipc::GpuController gpu(surface_host, marshal, &webview, {.backend = boson::gpu::Backend::D3D11});
gpu.register_content("terminal", [](boson::gpu::Surface& s) { s.set_frame_callback(...); });
boson::ipc::Router<boson::ipc::GpuController /*, ...*/> router(gpu);
boson::ipc::Bridge bridge(runner, marshal, router);
bridge.attach(webview);
gpu.connect(bridge);   // gpu:device_lost
```

Then in HTML:

```html
<boson-surface content="terminal" layer="below" style="position:fixed; inset:0"></boson-surface>
<boson-surface content="badge" layer="above" z-index="1" style="width:300px; height:64px"></boson-surface>
```

Attributes: `content` (required, registered name), `layer` (`below` default / `above`), `z-index`, `opaque`, `vsync="false"`. The element tracks its box with `ResizeObserver`, `IntersectionObserver`, scroll listeners and DPR changes, batches all elements into one `Gpu.set_bounds` per animation frame, and clips the native visual to `overflow` ancestors (scrollable containers). Events: `boson-surface-ready` (`detail` = surface info) and `boson-surface-error`. `element.surfaceId`, `element.requestFrame()`. Surfaces are destroyed with the element, and surfaces of a previous document are destroyed on navigation.

`frontend/src/gpu.ts` wraps the controller with types: `gpu.info()`, `createSurface(options, rect | element)`, `destroySurface(id)`, `setBounds(updates)`, `requestFrame(id)`, `onDeviceLost(fn)`, `whenReady()`, `mount(parent, options, style)`, plus the `HTMLElementTagNameMap` entry for `boson-surface`. There is no Electron equivalent, so `compat/electron.ts` is unchanged.

## Example

`boson_gpu_demo [--windowed] [--d3d12]` (`examples/gpu_demo`): a full-window animated build-log text grid (Renderer2D + glyph atlas, FPS and CPU time in the status bar) below a transparent page; a semi-transparent HTML panel with `Gpu.info`; a Vulkan surface (animated tiles rendered by Vulkan into a shared image, sampled by D3D) inside a scrollable card, clipped by it; and a native badge above the WebView that redraws only on demand.

## Performance (this machine)

AMD Radeon RX 9070 XT (FL 12_1), 180 Hz display, Windows 11 26300, Release build. From `boson_gpu_tests` (three runs, ranges) and the demo.

| Measurement | Result |
|---|---|
| `Device::create` D3D11 / D3D12 / WARP | 64 ms / 127 ms / 1.9 ms |
| Text grid 240x70 (17,150 quads, 2 draw calls), CPU only | 1.46 ms (D3D11), 1.72 ms (D3D12) |
| Same, CPU + GPU wait-idle | 1.7-3.1 ms (D3D11), 2.2-3.2 ms (D3D12) |
| `create_surface` → first present | 10-16 ms (windowed host), 2.9-4.0 ms (composition host) |
| 100 `request_frame()` calls | 1 frame |
| Idle 1 s after a frame | 0 presents, 0 render-thread wake-ups, 0 ms CPU |
| Animated (request every frame) | 180-181 fps (display rate) on D3D11 and D3D12, windowed and composition host |
| Render thread CPU per animated frame | ~0.26-0.39 ms; callback + present ~0.42-0.48 ms |
| Demo full-window terminal (1264x761) | 179-182 fps, 0.63-0.70 ms renderer CPU per frame |
| `VulkanInterop::create` | 433 ms first (D3D11) / 51 ms (D3D12) |
| Vulkan clear → D3D readback | 0.49 ms (D3D11) / 0.39 ms (D3D12), GPU-side fence sync |
| Device lost → recovered | 58-87 ms (D3D11, simulated) / 124-177 ms (D3D12, `RemoveDevice`) |

## Limitations

- Text: one font per atlas, no shaping (ligatures, complex scripts), no font fallback, no color emoji; a code point is one glyph. Fine for terminal / code grids in Latin, Cyrillic, Greek, box drawing; not a general text engine.
- `VulkanInterop` is not recovered automatically after device loss; recreate it on `on_device_lost` (`recovered == true`).
- The Vulkan/D3D protocol is sequential per image (`begin_vulkan` … `end_vulkan`, `acquire_d3d` … `release_d3d`); use two images for overlap.
- `Renderer2D` targets D3D11 render targets (on D3D12 via D3D11On12). Native D3D12 rendering uses `FrameContext::d3d12_back_buffer` directly.
- Windows 10/11 only. `DCompositionWaitForCompositorClock` needs Windows 11; on Windows 10 pacing falls back to the swap-chain waitable.

## Verification

Verified on this machine:

- `boson_gpu_tests` (ctest, 440/440 checks, three consecutive runs): adapter enumeration, device creation D3D11 and D3D12 (`HighPerformance`) and WARP, offscreen rendering readback (solid, rounded, glyph, text grid, texture pixels compared with tolerance), surface create/resize/present with readback of the swap-chain back buffer, request coalescing, idle zero-present / zero-wake / zero-CPU check, animation pacing, hidden and minimized pause/resume, device-lost recovery on D3D11 (simulated) and D3D12 (`ID3D12Device5::RemoveDevice`, a real device removal), Vulkan interop readback on D3D11 and D3D12, and `Gpu.*` dispatch through a real `Router` (create, set_bounds, request_frame, info, destroy).
- `boson_tests`, `boson_integration_tests`, `boson_titlebar_tests`, `boson_porting_tests` still pass after the `CompositionHost` change (underlay/overlay visuals).
- Demo screenshots (`PrintWindow` with `PW_RENDERFULLCONTENT`) in composition hosting (D3D11 and D3D12) and windowed hosting: the native terminal is visible through the transparent page and under the semi-transparent panel, the Vulkan surface is clipped by its scroll container, the badge is above the HTML. Window close exits cleanly (exit code 0, about 3.5 s including WebView2 shutdown).
- Full build with `/W4`, zero warnings; `bun run build` (tsc + vite) passes with `gpu.ts`.

Not verified:

- A real TDR / driver reset or GPU unplug (only `RemoveDevice` and the simulated D3D11 path).
- Moving the window between monitors with different DPI (the `WM_DPICHANGED` path runs, but only at one scale on this machine), real occlusion by other windows, and the `Default` / `MinimumPower` preferences or hybrid-GPU laptops (this machine has one hardware adapter).
- Machines without Vulkan (the skip path is covered by code review, `vulkan-1.dll` is present here) and drivers without timeline-semaphore import (the CPU-sync fallback was not exercised).
- Windows 10 (no compositor clock) and `vsync=false` tearing presents on a real fullscreen-like setup.
- `frontend/src/gpu.ts` is type-checked only; the demo drives the controller through the injected `<boson-surface>` runtime.
