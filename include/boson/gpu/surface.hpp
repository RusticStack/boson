#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <boson/gpu/device.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace boson::core {
class CompositionHost;
class WebViewHost;
class Window;
} // namespace boson::core

namespace boson::gpu {

class Surface;
class SurfaceHost;

namespace detail {
class SurfaceCore;

/// Only SurfaceHost can construct a Surface (make_unique needs a public constructor).
class SurfaceKey {
    friend class gpu::SurfaceHost;
    explicit SurfaceKey() = default;
};
} // namespace detail

/// Where a surface sits relative to the WebViews of its window.
enum class Layer : std::uint8_t {
    /// Behind every WebView; visible through transparent regions of the page (transparent_background = true).
    BelowWebView,
    /// On top of every WebView (overlays, drag previews). Takes no input: mouse input still reaches the page.
    AboveWebView,
};

struct SurfaceOptions {
    Layer layer = Layer::BelowWebView;
    int z_index = 0;         ///< order inside the layer (higher = closer to the viewer)
    bool opaque = false;     ///< DXGI_ALPHA_MODE_IGNORE; otherwise premultiplied alpha
    bool vsync = true;       ///< Present(1) - else Present(0)
    bool allow_tearing = false; ///< with vsync = false and DXGI tearing support (variable refresh rate)
    std::uint32_t buffer_count = 2;
    RECT bounds{0, 0, 1, 1}; ///< physical pixels, relative to the window client area
    float dpi_scale = 0.0f;  ///< 0 = the window's DPI / 96
};

struct SurfaceStats {
    std::uint64_t frames_presented = 0;
    std::uint64_t frames_requested = 0;   ///< request_frame() calls (coalesced into frames_presented)
    std::uint64_t resizes = 0;            ///< ResizeBuffers calls
    std::uint64_t swap_chain_creations = 0;
    std::uint64_t present_failures = 0;
    std::uint64_t occluded_presents = 0;
    double last_frame_cpu_ms = 0.0;       ///< frame callback + present on the render thread
};

/// @brief Everything a frame callback needs. Valid only during the callback (render thread, device lock held).
struct FrameContext {
    Surface& surface;
    Device& device;
    std::uint32_t width = 0;   ///< back buffer size in physical pixels
    std::uint32_t height = 0;
    float dpi_scale = 1.0f;
    std::uint64_t frame_index = 0;
    double time_seconds = 0.0;  ///< since the surface was created
    double delta_seconds = 0.0; ///< since the previous presented frame (0 for the first)
    /// The current back buffer as D3D11 (on D3D12 it is the D3D11On12 wrapped resource, already acquired).
    ID3D11Texture2D* back_buffer = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    /// D3D12 backend only: the raw back buffer (state PRESENT outside the wrapped D3D11 work) and its index.
    /// Native D3D12 renderers record their own lists on device.d3d12_queue() and transition PRESENT -> ... -> PRESENT.
    ID3D12Resource* d3d12_back_buffer = nullptr;
    std::uint32_t buffer_index = 0;
    /// Set to false to skip Present for this frame (nothing changed after all).
    bool present = true;
};

/// @brief One DXGI flip-model composition swap chain (CreateSwapChainForComposition, frame-latency waitable,
/// optional tearing) hosted in a DirectComposition visual of a SurfaceHost.
///
/// Rendering model: nothing happens until request_frame() (any thread). The device's render thread then waits
/// for the swap chain's waitable object, calls the frame callback and presents once - several requests made
/// before that frame coalesce into one. An animation calls request_frame() from its callback; it is paced
/// by the swap chain (vsync) and, on Windows 11, by the compositor clock. With no request the render thread
/// sleeps on a kernel event: zero CPU when idle. Minimized windows and occluded presents pause rendering.
///
/// Geometry setters are UI-thread only and are committed once per message-loop turn by the SurfaceHost.
/// Resizing re-creates the buffers at the new size on the render thread (no stretching): until the next frame
/// is presented the visual shows the previous content clipped to the new bounds.
class Surface {
public:
    using FrameCallback = std::function<void(FrameContext& frame)>;

    Surface(detail::SurfaceKey key, SurfaceHost& host, std::shared_ptr<Device> device, SurfaceOptions options,
            Microsoft::WRL::ComPtr<IDCompositionVisual2> visual, std::uint32_t id);
    /// @brief UI thread. Removes the visual; waits for a running frame callback of this surface to return.
    ~Surface();

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) = delete;
    Surface& operator=(Surface&&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept { return m_id; }
    [[nodiscard]] const std::shared_ptr<Device>& device() const noexcept { return m_device; }
    [[nodiscard]] SurfaceHost& host() const noexcept { return m_host; }

    /// @brief Any thread. Called on the render thread; must not block on the UI thread.
    void set_frame_callback(FrameCallback callback);
    /// @brief Any thread, lock-free. Requests one frame (coalesced).
    void request_frame() noexcept;

    // ---- Geometry (UI thread) ---------------------------------------------------------------------------
    /// @brief Physical pixels relative to the window client area. A new size resizes the swap chain.
    void set_bounds(RECT physical_px);
    /// @brief DIPs; re-applied automatically when the window's DPI changes.
    void set_bounds_dip(RECT dip);
    [[nodiscard]] RECT bounds() const noexcept { return m_bounds; }
    /// @brief Visible part in surface-local physical pixels (scroll containers); std::nullopt = whole surface.
    void set_clip(std::optional<RECT> local_px);
    void set_visible(bool visible);
    [[nodiscard]] bool is_visible() const noexcept { return m_visible; }
    void set_opacity(float opacity);
    void set_layer(Layer layer, int z_index = 0);
    [[nodiscard]] Layer layer() const noexcept { return m_layer; }
    [[nodiscard]] int z_index() const noexcept { return m_z_index; }
    /// @brief Scale handed to FrameContext::dpi_scale (0 = window DPI). JS-positioned surfaces pass devicePixelRatio.
    void set_dpi_scale(float scale);

    [[nodiscard]] SurfaceStats stats() const;
    [[nodiscard]] IDCompositionVisual2* visual() const noexcept { return m_visual.Get(); }

private:
    friend class SurfaceHost;

    void apply_geometry();
    void on_window_dpi(UINT dpi);
    void set_paused(bool paused) noexcept;

    SurfaceHost& m_host;
    std::shared_ptr<Device> m_device;
    std::shared_ptr<detail::SurfaceCore> m_core;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> m_visual;
    std::uint32_t m_id = 0;
    std::uint64_t m_order = 0;
    Layer m_layer = Layer::BelowWebView;
    int m_z_index = 0;
    RECT m_bounds{};
    std::optional<RECT> m_bounds_dip;
    std::optional<RECT> m_clip;
    bool m_visible = true;
    float m_opacity = 1.0f;
    float m_dpi_scale_override = 0.0f;
};

/// @brief The DirectComposition layers of one window that Surfaces live in. UI thread only (except post()).
///
/// Composition-hosted WebViews (HostingMode::Composition): surfaces go into the CompositionHost's underlay /
/// overlay visuals, below / above every WebView visual of the same tree. Windowed WebViews: the host creates
/// its own DirectComposition targets on the window HWND - topmost = FALSE (below the WebView child windows,
/// seen through transparent page regions) and, on demand, topmost = TRUE (above them).
class SurfaceHost {
public:
    explicit SurfaceHost(core::Window& window, std::shared_ptr<core::CompositionHost> composition = nullptr);
    /// @brief Uses host.composition() when the WebViews are composition hosted.
    explicit SurfaceHost(core::WebViewHost& host);
    ~SurfaceHost();

    SurfaceHost(const SurfaceHost&) = delete;
    SurfaceHost& operator=(const SurfaceHost&) = delete;
    SurfaceHost(SurfaceHost&&) = delete;
    SurfaceHost& operator=(SurfaceHost&&) = delete;

    /// @brief Creates the swap chain and its visual (UI thread). nullptr on failure.
    [[nodiscard]] std::unique_ptr<Surface> create_surface(std::shared_ptr<Device> device, SurfaceOptions options = {});

    [[nodiscard]] bool is_composition_hosted() const noexcept { return m_composition != nullptr; }
    [[nodiscard]] core::Window& window() const noexcept { return m_window; }
    [[nodiscard]] std::size_t surface_count() const noexcept { return m_surfaces.size(); }
    [[nodiscard]] Surface* find(std::uint32_t id) const noexcept;

    /// @brief Commits pending visual changes now (otherwise done once per message-loop turn).
    bool commit();
    /// @brief Runs fn on the UI thread (PostMessage to the window). Any thread.
    void post(std::function<void()> fn);

private:
    friend class Surface;

    struct LayerTree {
        Microsoft::WRL::ComPtr<IDCompositionTarget> target; ///< own-target mode only
        Microsoft::WRL::ComPtr<IDCompositionVisual2> root;
    };

    bool ensure_layer(Layer layer);
    Microsoft::WRL::ComPtr<IDCompositionVisual2> create_visual();
    [[nodiscard]] IDCompositionDesktopDevice* dcomp() const noexcept;
    void rebuild_layer(Layer layer);
    void unregister(Surface* surface) noexcept;
    void schedule_commit();
    void drain_posted();
    void on_minimized(bool minimized);
    void on_dpi(UINT dpi);

    core::Window& m_window;
    std::shared_ptr<core::CompositionHost> m_composition;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> m_device; ///< own-target mode
    LayerTree m_below;
    LayerTree m_above;
    std::vector<Surface*> m_surfaces;
    std::uint32_t m_next_id = 1;
    std::uint64_t m_next_order = 1;
    bool m_commit_scheduled = false;
    bool m_minimized = false;

    UINT m_post_message = 0;
    std::uint64_t m_hook = 0;
    std::vector<std::uint64_t> m_listeners;
    std::shared_ptr<std::mutex> m_post_mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::function<void()>>> m_posted = std::make_shared<std::vector<std::function<void()>>>();
};

} // namespace boson::gpu
