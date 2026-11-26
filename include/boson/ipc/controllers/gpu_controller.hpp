#pragma once

#include <windows.h>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/gpu/device.hpp>
#include <boson/gpu/surface.hpp>
#include <boson/gpu/vulkan_interop.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::ipc {

/// A rectangle in CSS pixels, relative to the WebView viewport (getBoundingClientRect()).
struct GpuRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

struct GpuCreateSurfaceRequest {
    std::string content;          ///< name registered with GpuController::register_content
    std::string layer = "below";  ///< "below" | "above" the WebView
    int z_index = 0;
    GpuRect rect;
    double scale = 1.0;           ///< window.devicePixelRatio
    bool opaque = false;
    bool vsync = true;
    std::string page;             ///< per-document token: surfaces of an older document of the same WebView are destroyed
};

struct GpuSurfaceInfo {
    std::uint32_t id = 0;
    std::uint32_t width = 0;  ///< physical pixels
    std::uint32_t height = 0;
    std::string content;
    std::string layer;
};

struct GpuBoundsUpdate {
    std::uint32_t id = 0;
    GpuRect rect;
    std::optional<GpuRect> clip; ///< visible part, element-local CSS px (scroll containers); null = all
    bool visible = true;
};

struct GpuSetBoundsRequest {
    double scale = 1.0; ///< window.devicePixelRatio
    std::vector<GpuBoundsUpdate> surfaces;
};

struct GpuSurfaceRef {
    std::uint32_t id = 0;
};

struct GpuOk {
    bool ok = false;
};

struct GpuAdapter {
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t dedicated_video_memory_mb = 0;
    bool software = false;
};

struct GpuInfo {
    std::string backend;            ///< "d3d11" | "d3d12"
    GpuAdapter adapter;
    std::string feature_level;      ///< "12_1", ...
    bool tearing_supported = false;
    bool compositor_clock = false;  ///< frames paced by DCompositionWaitForCompositorClock
    bool vulkan_available = false;  ///< vulkan-1.dll loaded (interop itself is created by the app)
    std::string vulkan_version;
    std::string hosting;            ///< "composition" | "windowed"
    std::uint64_t device_generation = 0;
    std::uint32_t surfaces = 0;
    std::vector<std::string> contents;
    std::vector<GpuAdapter> adapters;
};

/// "gpu:device_lost": the GPU device was removed / reset (driver update, TDR, adapter unplugged). Surfaces are
/// re-created automatically when recovered is true.
struct GpuDeviceLostEvent {
    std::string reason; ///< HRESULT name, e.g. "DXGI_ERROR_DEVICE_REMOVED"
    bool recovered = false;
    std::uint64_t generation = 0;
};

/// Event registry for TsGenerator::generate_with_events.
struct GpuEvents {
    GpuDeviceLostEvent device_lost;
};

/// @brief JS controller "Gpu": native GPU surfaces positioned by DOM elements (<boson-surface>).
///
/// The C++ app registers named content providers (register_content) that attach a frame callback to each new
/// Surface; the page creates surfaces by name and keeps their bounds in sync. The Device is created lazily on
/// the first call (pool thread). Surface objects live on the UI thread: create/destroy marshal there with
/// invoke_sync, set_bounds is posted (non-blocking). Destroy the controller on the UI thread, before the
/// SurfaceHost.
class GpuController {
public:
    BOSON_CONTROLLER("Gpu");

    static constexpr std::string_view kDeviceLostEvent = "gpu:device_lost";
    using ContentFactory = std::function<void(gpu::Surface& surface)>;

    GpuController(gpu::SurfaceHost& host, exec::UiMarshal& marshal, core::WebView* webview = nullptr,
                  gpu::DeviceOptions options = {})
        : m_host(host), m_marshal(marshal), m_webview(webview), m_options(options) {}

    GpuController(const GpuController&) = delete;
    GpuController& operator=(const GpuController&) = delete;

    ~GpuController() {
        std::shared_ptr<gpu::Device> device;
        {
            std::scoped_lock guard(m_mutex);
            device = m_device;
        }
        if (device && m_lost_listener != 0) {
            device->off_device_lost(m_lost_listener);
        }
        m_surfaces.clear(); // UI thread
    }

    // ---- C++ API ----------------------------------------------------------------------------------------
    /// @brief Registers a content provider: called on the UI thread with each new Surface of that name
    /// (set its frame callback there). Any thread.
    void register_content(std::string name, ContentFactory factory) {
        std::scoped_lock guard(m_mutex);
        m_contents[std::move(name)] = std::move(factory);
    }

    /// @brief Uses an existing device instead of creating one lazily (before the first call).
    void set_device(std::shared_ptr<gpu::Device> device) {
        std::scoped_lock guard(m_mutex);
        m_device = std::move(device);
        subscribe_locked();
    }

    /// @brief The shared device (created on first use; nullptr when no adapter works). Any thread.
    [[nodiscard]] std::shared_ptr<gpu::Device> device() {
        std::scoped_lock guard(m_mutex);
        if (!m_device && !m_device_failed) {
            m_device = gpu::Device::create(m_options);
            m_device_failed = !m_device;
            subscribe_locked();
        }
        return m_device;
    }

    /// @brief Forwards device-lost to the page as "gpu:device_lost" (the bridge must outlive the controller).
    void connect(Bridge& bridge) {
        std::scoped_lock guard(m_mutex);
        m_bridge = &bridge;
    }

    /// @brief The C++ Surface behind a JS surface id (UI thread).
    [[nodiscard]] gpu::Surface* surface(std::uint32_t id) const noexcept {
        for (const auto& entry : m_surfaces) {
            if (entry.surface && entry.surface->id() == id) {
                return entry.surface.get();
            }
        }
        return nullptr;
    }

    /// @brief Installs the <boson-surface> element runtime (window.__bosonGpu) in webview (UI thread).
    static void attach(core::WebView& webview) {
        const std::string script = core::wide_to_utf8(page_script());
        webview.add_script_on_document_created(script, [](HRESULT, std::string) {});
        webview.execute_script(script, [](HRESULT, std::string) {});
    }

    /// @brief The page runtime: defines <boson-surface content="name" layer="below|above" z-index opaque>.
    /// Each element creates a surface when connected and destroys it when disconnected. ResizeObserver,
    /// IntersectionObserver, scroll (capture) and devicePixelRatio changes schedule one batched
    /// Gpu.set_bounds per animation frame for every element that moved.
    [[nodiscard]] static std::wstring page_script();

    // ---- JS API -----------------------------------------------------------------------------------------
    [[nodiscard]] GpuSurfaceInfo create_surface(GpuCreateSurfaceRequest req) {
        if (req.layer != "below" && req.layer != "above") {
            throw std::invalid_argument("layer must be \"below\" or \"above\"");
        }
        ContentFactory factory;
        {
            std::scoped_lock guard(m_mutex);
            const auto it = m_contents.find(req.content);
            if (it == m_contents.end()) {
                throw std::invalid_argument("unknown surface content: " + req.content);
            }
            factory = it->second;
        }
        std::shared_ptr<gpu::Device> dev = device();
        if (!dev) {
            throw std::runtime_error("no GPU device available");
        }
        const std::uint64_t sender = current_sender_id();
        return m_marshal.invoke_sync([this, &req, &factory, &dev, sender]() {
            if (!req.page.empty()) {
                std::erase_if(m_surfaces, [&](const Entry& e) { return e.sender == sender && !e.page.empty() && e.page != req.page; });
            }
            gpu::SurfaceOptions options;
            options.layer = req.layer == "above" ? gpu::Layer::AboveWebView : gpu::Layer::BelowWebView;
            options.z_index = req.z_index;
            options.opaque = req.opaque;
            options.vsync = req.vsync;
            options.dpi_scale = static_cast<float>(std::max(0.25, req.scale));
            options.bounds = to_physical(req.rect, req.scale);
            auto surface = m_host.create_surface(dev, options);
            if (!surface) {
                throw std::runtime_error("could not create the surface");
            }
            factory(*surface);
            surface->request_frame();
            GpuSurfaceInfo info{
                .id = surface->id(),
                .width = static_cast<std::uint32_t>(options.bounds.right - options.bounds.left),
                .height = static_cast<std::uint32_t>(options.bounds.bottom - options.bounds.top),
                .content = req.content,
                .layer = req.layer,
            };
            m_surfaces.push_back(Entry{.surface = std::move(surface), .sender = sender, .page = req.page});
            return info;
        });
    }

    [[nodiscard]] GpuOk destroy(GpuSurfaceRef req) {
        return GpuOk{.ok = m_marshal.invoke_sync([this, id = req.id]() {
            return std::erase_if(m_surfaces, [id](const Entry& e) { return e.surface && e.surface->id() == id; }) > 0;
        })};
    }

    /// Batched per animation frame by the page runtime. Posted to the UI thread; never blocks the caller.
    [[nodiscard]] GpuOk set_bounds(GpuSetBoundsRequest req) {
        m_marshal.post_task([this, req = std::move(req)]() {
            for (const GpuBoundsUpdate& update : req.surfaces) {
                gpu::Surface* s = surface(update.id);
                if (s == nullptr) {
                    continue;
                }
                s->set_dpi_scale(static_cast<float>(std::max(0.25, req.scale)));
                s->set_bounds(to_physical(update.rect, req.scale));
                if (update.clip.has_value()) {
                    const GpuRect& c = *update.clip;
                    s->set_clip(RECT{static_cast<LONG>(std::floor(c.x * req.scale)), static_cast<LONG>(std::floor(c.y * req.scale)),
                                     static_cast<LONG>(std::ceil((c.x + c.width) * req.scale)),
                                     static_cast<LONG>(std::ceil((c.y + c.height) * req.scale))});
                } else {
                    s->set_clip(std::nullopt);
                }
                s->set_visible(update.visible);
            }
        });
        return GpuOk{.ok = true};
    }

    [[nodiscard]] GpuOk request_frame(GpuSurfaceRef req) {
        return GpuOk{.ok = m_marshal.invoke_sync([this, id = req.id]() {
            gpu::Surface* s = surface(id);
            if (s != nullptr) {
                s->request_frame();
            }
            return s != nullptr;
        })};
    }

    [[nodiscard]] GpuInfo info() {
        GpuInfo out;
        if (const std::shared_ptr<gpu::Device> dev = device()) {
            const gpu::DeviceInfo d = dev->info();
            out.backend = d.backend;
            out.adapter = to_adapter(d.adapter);
            out.feature_level = d.feature_level;
            out.tearing_supported = d.tearing_supported;
            out.compositor_clock = d.compositor_clock;
            out.device_generation = d.generation;
        }
        const gpu::VulkanSupport& vk = gpu::vulkan_support();
        out.vulkan_available = vk.loader;
        out.vulkan_version = vk.version_string();
        out.hosting = m_host.is_composition_hosted() ? "composition" : "windowed";
        out.surfaces = m_marshal.invoke_sync([this]() { return static_cast<std::uint32_t>(m_surfaces.size()); });
        {
            std::scoped_lock guard(m_mutex);
            for (const auto& [name, factory] : m_contents) {
                out.contents.push_back(name);
            }
        }
        for (const gpu::AdapterInfo& a : gpu::Device::enumerate_adapters()) {
            out.adapters.push_back(to_adapter(a));
        }
        return out;
    }

private:
    struct Entry {
        std::unique_ptr<gpu::Surface> surface;
        std::uint64_t sender = 0;
        std::string page;
    };

    static GpuAdapter to_adapter(const gpu::AdapterInfo& a) {
        return GpuAdapter{
            .name = a.name,
            .vendor_id = a.vendor_id,
            .device_id = a.device_id,
            .dedicated_video_memory_mb = a.dedicated_video_memory / (1024ull * 1024ull),
            .software = a.software,
        };
    }

    /// UI thread: CSS px (viewport) -> physical px in the window client area.
    RECT to_physical(const GpuRect& r, double scale) const {
        POINT origin{0, 0};
        if (m_webview != nullptr) {
            const RECT wv = m_webview->bounds();
            origin = POINT{wv.left, wv.top};
        }
        const auto px = [scale](double v) { return static_cast<LONG>(std::lround(v * scale)); };
        RECT out{px(r.x) + origin.x, px(r.y) + origin.y, px(r.x + r.width) + origin.x, px(r.y + r.height) + origin.y};
        out.right = std::max(out.right, out.left + 1);
        out.bottom = std::max(out.bottom, out.top + 1);
        return out;
    }

    void subscribe_locked() {
        if (!m_device || m_lost_listener != 0) {
            return;
        }
        m_lost_listener = m_device->on_device_lost([this](const gpu::DeviceLostInfo& info) {
            Bridge* bridge = nullptr;
            {
                std::scoped_lock guard(m_mutex);
                bridge = m_bridge;
            }
            if (bridge != nullptr) {
                bridge->emit(kDeviceLostEvent, GpuDeviceLostEvent{
                    .reason = gpu::hresult_name(info.reason),
                    .recovered = info.recovered,
                    .generation = info.generation,
                });
            }
        });
    }

    gpu::SurfaceHost& m_host;
    exec::UiMarshal& m_marshal;
    core::WebView* m_webview = nullptr;
    gpu::DeviceOptions m_options;

    std::mutex m_mutex;
    std::shared_ptr<gpu::Device> m_device;
    bool m_device_failed = false;
    gpu::Device::ListenerId m_lost_listener = 0;
    Bridge* m_bridge = nullptr;
    std::map<std::string, ContentFactory, std::less<>> m_contents;

    std::vector<Entry> m_surfaces; ///< UI thread only
};

inline std::wstring GpuController::page_script() {
    return LR"JS((() => {
  'use strict';
  if (window.__bosonGpu) return;
  const wv = window.chrome && window.chrome.webview;
  if (!wv) return;
  // Request ids far above boson.ts' counter so both clients can share the message channel.
  let nextId = 0x4000000000000;
  const pending = new Map();
  wv.addEventListener('message', (event) => {
    let msg = event.data;
    if (typeof msg === 'string') { try { msg = JSON.parse(msg); } catch { return; } }
    if (!msg || typeof msg.id !== 'number' || !pending.has(msg.id)) return;
    const p = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.status === 'success') p.resolve(msg.data);
    else p.reject(new Error((msg.error && msg.error.message) || 'Gpu call failed'));
  });
  const call = (method, params) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    try { wv.postMessage({ id, method, params: params || {} }); } catch (err) { pending.delete(id); reject(err); }
  });
  const page = Math.random().toString(36).slice(2) + Date.now().toString(36);
  const dirty = new Set();
  const live = new Set();
  let frame = 0;

  const clipOf = (el, rect) => {
    let left = Math.max(rect.left, 0), top = Math.max(rect.top, 0);
    let right = Math.min(rect.right, innerWidth), bottom = Math.min(rect.bottom, innerHeight);
    for (let p = el.parentElement; p && p !== document.documentElement; p = p.parentElement) {
      const cs = getComputedStyle(p);
      if (cs.overflowX !== 'visible' || cs.overflowY !== 'visible' || cs.contain.includes('paint')) {
        const r = p.getBoundingClientRect();
        left = Math.max(left, r.left + p.clientLeft); top = Math.max(top, r.top + p.clientTop);
        right = Math.min(right, r.left + p.clientLeft + p.clientWidth); bottom = Math.min(bottom, r.top + p.clientTop + p.clientHeight);
      }
    }
    if (right <= left || bottom <= top) return null;
    return { x: left - rect.left, y: top - rect.top, width: right - left, height: bottom - top };
  };

  const flush = () => {
    frame = 0;
    const surfaces = [];
    for (const el of dirty) {
      if (!el._bosonId) continue;
      const rect = el.getBoundingClientRect();
      const clip = clipOf(el, rect);
      const visible = el._bosonIntersecting !== false && clip !== null && rect.width > 0 && rect.height > 0 &&
        document.visibilityState === 'visible';
      const full = clip && clip.x === 0 && clip.y === 0 && clip.width === rect.width && clip.height === rect.height;
      surfaces.push({ id: el._bosonId, rect: { x: rect.left, y: rect.top, width: rect.width, height: rect.height },
        clip: full ? null : clip, visible });
    }
    dirty.clear();
    if (surfaces.length) call('Gpu.set_bounds', { scale: devicePixelRatio, surfaces }).catch(() => {});
  };
  const schedule = (el) => {
    if (el) dirty.add(el); else for (const e of live) dirty.add(e);
    if (!frame) frame = requestAnimationFrame(flush);
  };

  const resizeObserver = new ResizeObserver((entries) => { for (const e of entries) schedule(e.target); });
  const intersectionObserver = new IntersectionObserver((entries) => {
    for (const e of entries) { e.target._bosonIntersecting = e.isIntersecting; schedule(e.target); }
  });
  addEventListener('scroll', () => schedule(null), { capture: true, passive: true });
  addEventListener('resize', () => schedule(null), { passive: true });
  document.addEventListener('visibilitychange', () => schedule(null));
  const watchDpr = () => {
    matchMedia('(resolution: ' + devicePixelRatio + 'dppx)').addEventListener('change', () => { schedule(null); watchDpr(); }, { once: true });
  };
  watchDpr();

  class BosonSurfaceElement extends HTMLElement {
    static get observedAttributes() { return ['content', 'layer', 'z-index', 'opaque']; }
    get surfaceId() { return this._bosonId || 0; }
    connectedCallback() {
      if (!this.style.display) this.style.display = 'block';
      live.add(this);
      resizeObserver.observe(this);
      intersectionObserver.observe(this);
      this._create();
    }
    disconnectedCallback() {
      live.delete(this);
      dirty.delete(this);
      resizeObserver.unobserve(this);
      intersectionObserver.unobserve(this);
      this._destroy();
    }
    attributeChangedCallback(name, oldValue, newValue) {
      if (oldValue !== newValue && this.isConnected && this._bosonId) { this._destroy(); this._create(); }
    }
    requestFrame() { return this._bosonId ? call('Gpu.request_frame', { id: this._bosonId }) : Promise.resolve({ ok: false }); }
    _create() {
      const content = this.getAttribute('content');
      if (!content) return;
      const token = (this._bosonToken = (this._bosonToken || 0) + 1);
      const rect = this.getBoundingClientRect();
      call('Gpu.create_surface', {
        content, layer: this.getAttribute('layer') || 'below', z_index: Number(this.getAttribute('z-index') || 0),
        rect: { x: rect.left, y: rect.top, width: rect.width, height: rect.height }, scale: devicePixelRatio,
        opaque: this.hasAttribute('opaque'), vsync: this.getAttribute('vsync') !== 'false', page,
      }).then((info) => {
        if (token !== this._bosonToken || !this.isConnected) { call('Gpu.destroy', { id: info.id }).catch(() => {}); return; }
        this._bosonId = info.id;
        schedule(this);
        this.dispatchEvent(new CustomEvent('boson-surface-ready', { detail: info, bubbles: true }));
      }).catch((err) => {
        this.dispatchEvent(new CustomEvent('boson-surface-error', { detail: String(err && err.message || err), bubbles: true }));
      });
    }
    _destroy() {
      this._bosonToken = (this._bosonToken || 0) + 1;
      const id = this._bosonId;
      this._bosonId = 0;
      if (id) call('Gpu.destroy', { id }).catch(() => {});
    }
  }
  if (!customElements.get('boson-surface')) customElements.define('boson-surface', BosonSurfaceElement);
  window.__bosonGpu = { version: 1, page, call, refresh: () => schedule(null) };
})();)JS";
}

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::GpuEvents> {
    using T = boson::ipc::GpuEvents;
    static constexpr auto value = glz::object("gpu:device_lost", &T::device_lost);
};

template <>
struct glz::meta<boson::ipc::GpuController> {
    using T = boson::ipc::GpuController;
    static constexpr auto value = glz::object(
        "create_surface", &T::create_surface,
        "destroy", &T::destroy,
        "set_bounds", &T::set_bounds,
        "request_frame", &T::request_frame,
        "info", &T::info);
};
