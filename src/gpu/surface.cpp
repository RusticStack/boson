#include "surface_core.hpp"

#include <boson/gpu/render_loop.hpp>

#include <boson/core/composition.hpp>
#include <boson/core/result.hpp>
#include <boson/core/webview_host.hpp>
#include <boson/core/window.hpp>

#include <algorithm>
#include <exception>

namespace boson::gpu {

using Microsoft::WRL::ComPtr;

namespace detail {

namespace {

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

std::uint64_t pack_size(std::uint32_t width, std::uint32_t height) noexcept {
    return (static_cast<std::uint64_t>(width) << 32) | height;
}

} // namespace

SurfaceCore::SurfaceCore(Device& device, const SurfaceOptions& options, Surface* owner)
    : m_device(device)
    , m_owner(owner)
    , m_options(options) {
    m_options.buffer_count = std::clamp(m_options.buffer_count, 2u, 4u);
    QueryPerformanceFrequency(&m_qpc_frequency);
    QueryPerformanceCounter(&m_created);
    const auto w = static_cast<std::uint32_t>(std::max<LONG>(1, options.bounds.right - options.bounds.left));
    const auto h = static_cast<std::uint32_t>(std::max<LONG>(1, options.bounds.bottom - options.bounds.top));
    m_requested_size.store(pack_size(w, h), std::memory_order_relaxed);
    m_device.add_resource_owner(this);
}

SurfaceCore::~SurfaceCore() {
    m_device.remove_resource_owner(this);
    const auto guard = m_device.lock();
    release_views();
    m_swap_chain3.Reset();
    m_swap_chain.Reset();
    if (m_waitable != nullptr) {
        CloseHandle(m_waitable);
    }
    close_retired();
}

void SurfaceCore::request_frame() noexcept {
    m_requests.fetch_add(1, std::memory_order_relaxed);
    if (!m_dirty.exchange(true, std::memory_order_acq_rel)) {
        m_device.render_loop().wake();
    }
}

void SurfaceCore::set_size(std::uint32_t width, std::uint32_t height) noexcept {
    const std::uint64_t packed = pack_size(std::max(1u, width), std::max(1u, height));
    if (m_requested_size.exchange(packed, std::memory_order_acq_rel) != packed) {
        request_frame();
    }
}

void SurfaceCore::set_pause(std::uint32_t bit, bool on) noexcept {
    if (on) {
        m_pause.fetch_or(bit, std::memory_order_acq_rel);
        return;
    }
    const std::uint32_t before = m_pause.fetch_and(~bit, std::memory_order_acq_rel);
    if ((before & bit) != 0 && (before & ~bit) == 0) {
        // The content may be stale (resized / DPI changed while paused). Wake explicitly: a request made while
        // paused already set the dirty flag, so request_frame() alone would not.
        m_dirty.store(true, std::memory_order_release);
        m_device.render_loop().wake();
    }
}

void SurfaceCore::set_callback(Surface::FrameCallback callback) {
    Surface::FrameCallback old;
    {
        std::scoped_lock guard(m_callback_mutex);
        if (m_closing.load(std::memory_order_acquire)) {
            return;
        }
        old = std::exchange(m_callback, std::move(callback));
    }
    // old is destroyed here, outside the lock (it may own renderers that unregister from the device).
}

void SurfaceCore::set_content_fn(SetContentFn fn) {
    std::scoped_lock guard(m_callback_mutex);
    m_set_content = std::move(fn);
}

void SurfaceCore::close() {
    Surface::FrameCallback old;
    {
        std::scoped_lock guard(m_callback_mutex);
        m_closing.store(true, std::memory_order_release);
        m_owner = nullptr;
        old = std::move(m_callback);
        m_callback = nullptr;
        m_set_content = nullptr;
    }
}

SurfaceStats SurfaceCore::stats() const {
    std::scoped_lock guard(m_stats_mutex);
    SurfaceStats out = m_stats;
    out.frames_requested = m_requests.load(std::memory_order_relaxed);
    return out;
}

bool SurfaceCore::wants_frame() const noexcept {
    return !m_closing.load(std::memory_order_acquire) && m_dirty.load(std::memory_order_acquire) &&
           m_pause.load(std::memory_order_acquire) == 0;
}

bool SurfaceCore::ready_without_wait() const noexcept {
    return m_slot || !m_swap_chain || m_waitable == nullptr || m_generation != m_device.generation();
}

bool SurfaceCore::try_acquire(bool already_signaled) noexcept {
    if (m_slot) {
        return true;
    }
    if (already_signaled || m_waitable == nullptr || WaitForSingleObject(m_waitable, 0) == WAIT_OBJECT_0) {
        m_slot = true;
    }
    return m_slot;
}

void SurfaceCore::probe_occlusion() noexcept {
    if (m_swap_chain && m_swap_chain->Present(0, DXGI_PRESENT_TEST) != DXGI_STATUS_OCCLUDED) {
        m_occluded = false;
    }
}

void SurfaceCore::close_retired() noexcept {
    for (HANDLE& handle : m_retired) {
        if (handle != nullptr) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }
}

void SurfaceCore::release_views() noexcept {
    m_rtv.Reset();
    m_back_buffer.Reset();
    for (auto& rtv : m_wrapped_rtv) {
        rtv.Reset();
    }
    for (auto& wrapped : m_wrapped) {
        wrapped.Reset();
    }
    for (auto& buffer : m_d3d12_buffers) {
        buffer.Reset();
    }
    if (ID3D11DeviceContext* context = m_device.d3d11_context(); context != nullptr) {
        // Views must be unbound (and deferred destruction flushed) before ResizeBuffers.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->Flush();
    }
}

void SurfaceCore::release_device_resources() noexcept {
    release_views();
    m_swap_chain3.Reset();
    m_swap_chain.Reset();
    if (m_waitable != nullptr) {
        // The render thread may be blocked on it right now: close it on that thread, later.
        for (HANDLE& slot : m_retired) {
            if (slot == nullptr) {
                slot = m_waitable;
                m_waitable = nullptr;
                break;
            }
        }
        if (m_waitable != nullptr) {
            CloseHandle(m_waitable);
            m_waitable = nullptr;
        }
    }
    m_slot = false;
    m_generation = ~0ull;
    SetContentFn set_content;
    {
        std::scoped_lock guard(m_callback_mutex);
        set_content = m_set_content;
    }
    if (set_content) {
        set_content(nullptr); // the visual must let go of the old swap chain (and with it the old device)
    }
    m_dirty.store(true, std::memory_order_release);
}

bool SurfaceCore::ensure_swap_chain() {
    if (m_swap_chain && m_generation == m_device.generation()) {
        return true;
    }
    if (m_swap_chain) {
        release_device_resources();
    }
    IDXGIFactory2* factory = m_device.dxgi_factory();
    IUnknown* target = m_device.swap_chain_device();
    if (factory == nullptr || target == nullptr) {
        return false;
    }
    const std::uint64_t packed = m_requested_size.load(std::memory_order_acquire);
    m_width = static_cast<std::uint32_t>(packed >> 32);
    m_height = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);

    m_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_options.allow_tearing && !m_options.vsync && m_device.tearing_supported() && m_tearing_ok) {
        m_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = kFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = m_options.buffer_count;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = m_options.opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = m_flags;

    ComPtr<IDXGISwapChain1> swap_chain;
    HRESULT hr = factory->CreateSwapChainForComposition(target, &desc, nullptr, &swap_chain);
    if (FAILED(hr) && (m_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
        m_tearing_ok = false;
        m_flags &= ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        desc.Flags = m_flags;
        hr = factory->CreateSwapChainForComposition(target, &desc, nullptr, &swap_chain);
    }
    if (!BOSON_HR_CHECK(hr)) {
        std::scoped_lock guard(m_stats_mutex);
        ++m_stats.present_failures;
        return false;
    }
    ComPtr<IDXGISwapChain2> swap_chain2;
    if (SUCCEEDED(swap_chain.As(&swap_chain2))) {
        swap_chain2->SetMaximumFrameLatency(1);
        m_waitable = swap_chain2->GetFrameLatencyWaitableObject();
    }
    swap_chain.As(&m_swap_chain3);
    m_swap_chain = swap_chain;
    m_generation = m_device.generation();
    m_slot = false;
    {
        std::scoped_lock guard(m_stats_mutex);
        ++m_stats.swap_chain_creations;
    }
    SetContentFn set_content;
    {
        std::scoped_lock guard(m_callback_mutex);
        set_content = m_set_content;
    }
    if (set_content) {
        set_content(m_swap_chain);
    }
    return true;
}

bool SurfaceCore::resize_if_needed() {
    const std::uint64_t packed = m_requested_size.load(std::memory_order_acquire);
    const auto width = static_cast<std::uint32_t>(packed >> 32);
    const auto height = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
    if (width == m_width && height == m_height) {
        return true;
    }
    release_views();
    const HRESULT hr = m_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_flags);
    if (!BOSON_HR_CHECK(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            m_device.handle_device_lost(hr);
        }
        return false;
    }
    m_width = width;
    m_height = height;
    std::scoped_lock guard(m_stats_mutex);
    ++m_stats.resizes;
    return true;
}

HRESULT SurfaceCore::render() {
    std::unique_lock callback_lock(m_callback_mutex);
    if (m_closing.load(std::memory_order_acquire) || m_owner == nullptr) {
        return S_OK;
    }
    callback_lock.unlock(); // ensure_swap_chain() takes it to read m_set_content
    if (!ensure_swap_chain() || !resize_if_needed()) {
        m_dirty.store(false, std::memory_order_release); // no retry loop: the next request_frame() tries again
        return S_OK;
    }
    if (!try_acquire(false)) {
        return S_OK; // the loop waits for the waitable next time
    }
    callback_lock.lock();
    if (m_closing.load(std::memory_order_acquire) || m_owner == nullptr) {
        return S_OK;
    }

    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    m_dirty.store(false, std::memory_order_release); // requests made during the callback schedule the next frame

    const bool d3d12 = m_device.backend() == Backend::D3D12;
    ID3D11Texture2D* back_buffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D12Resource* raw12 = nullptr;
    std::uint32_t index = 0;
    ID3D11Device* device11 = m_device.d3d11();
    if (d3d12) {
        index = m_swap_chain3 ? m_swap_chain3->GetCurrentBackBufferIndex() : 0;
        index = std::min<std::uint32_t>(index, static_cast<std::uint32_t>(m_wrapped.size() - 1));
        if (!m_wrapped[index]) {
            ComPtr<ID3D12Resource> buffer;
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (!BOSON_HR_CHECK(m_swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffer))) ||
                !BOSON_HR_CHECK(m_device.d3d11on12()->CreateWrappedResource(buffer.Get(), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                                                             D3D12_RESOURCE_STATE_PRESENT,
                                                                             IID_PPV_ARGS(&m_wrapped[index]))) ||
                !BOSON_HR_CHECK(device11->CreateRenderTargetView(m_wrapped[index].Get(), nullptr, &m_wrapped_rtv[index]))) {
                m_wrapped[index].Reset();
                return S_OK;
            }
            m_d3d12_buffers[index] = buffer;
        }
        ID3D11Resource* wrapped[] = {m_wrapped[index].Get()};
        m_device.d3d11on12()->AcquireWrappedResources(wrapped, 1);
        back_buffer = m_wrapped[index].Get();
        rtv = m_wrapped_rtv[index].Get();
        raw12 = m_d3d12_buffers[index].Get();
    } else {
        if (!m_rtv) {
            if (!BOSON_HR_CHECK(m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&m_back_buffer))) ||
                !BOSON_HR_CHECK(device11->CreateRenderTargetView(m_back_buffer.Get(), nullptr, &m_rtv))) {
                m_back_buffer.Reset();
                return S_OK;
            }
        }
        back_buffer = m_back_buffer.Get();
        rtv = m_rtv.Get();
    }

    FrameContext frame{
        .surface = *m_owner,
        .device = m_device,
        .width = m_width,
        .height = m_height,
        .dpi_scale = m_dpi_scale.load(std::memory_order_relaxed),
        .frame_index = m_frame_index,
        .time_seconds = static_cast<double>(start.QuadPart - m_created.QuadPart) / static_cast<double>(m_qpc_frequency.QuadPart),
        .delta_seconds = m_last_present.QuadPart == 0
                             ? 0.0
                             : static_cast<double>(start.QuadPart - m_last_present.QuadPart) / static_cast<double>(m_qpc_frequency.QuadPart),
        .back_buffer = back_buffer,
        .render_target = rtv,
        .d3d12_back_buffer = raw12,
        .buffer_index = index,
        .present = true,
    };
    if (m_callback) {
        try {
            m_callback(frame);
        } catch (const std::exception& ex) {
            core::hr_check(E_FAIL, ex.what());
        } catch (...) {
            core::hr_check(E_FAIL, "gpu::Surface frame callback threw");
        }
    }
    callback_lock.unlock();

    if (d3d12) {
        ID3D11Resource* wrapped[] = {m_wrapped[index].Get()};
        m_device.d3d11on12()->ReleaseWrappedResources(wrapped, 1);
        m_device.d3d11_context()->Flush();
    }

    HRESULT hr = S_OK;
    if (frame.present) {
        const UINT sync = m_options.vsync ? 1u : 0u;
        UINT flags = 0;
        if (sync == 0 && (m_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
            flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
        hr = m_swap_chain->Present(sync, flags);
        if (hr == DXGI_ERROR_INVALID_CALL && flags != 0) {
            m_tearing_ok = false; // not allowed for this swap chain: present without tearing from now on
            hr = m_swap_chain->Present(sync, 0);
        }
        m_slot = false;
        m_last_present = start;
        ++m_frame_index;
    }

    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    {
        std::scoped_lock guard(m_stats_mutex);
        if (frame.present && SUCCEEDED(hr)) {
            ++m_stats.frames_presented;
        }
        if (hr == DXGI_STATUS_OCCLUDED) {
            ++m_stats.occluded_presents;
        }
        if (FAILED(hr)) {
            ++m_stats.present_failures;
        }
        m_stats.last_frame_cpu_ms =
            static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(m_qpc_frequency.QuadPart);
    }
    if (hr == DXGI_STATUS_OCCLUDED) {
        m_occluded = true;
    }
    return hr;
}

} // namespace detail

// ---- Surface ---------------------------------------------------------------------------------------------

Surface::Surface(detail::SurfaceKey, SurfaceHost& host, std::shared_ptr<Device> device, SurfaceOptions options,
                 ComPtr<IDCompositionVisual2> visual, std::uint32_t id)
    : m_host(host)
    , m_device(std::move(device))
    , m_visual(std::move(visual))
    , m_id(id)
    , m_layer(options.layer)
    , m_z_index(options.z_index)
    , m_bounds(options.bounds)
    , m_dpi_scale_override(options.dpi_scale) {
    m_core = std::make_shared<detail::SurfaceCore>(*m_device, options, this);
    m_core->set_dpi_scale(options.dpi_scale > 0.0f ? options.dpi_scale
                                                   : static_cast<float>(m_host.window().dpi()) / static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    SurfaceHost* host_ptr = &m_host;
    ComPtr<IDCompositionVisual2> visual_ref = m_visual;
    m_core->set_content_fn([host_ptr, visual_ref](ComPtr<IDXGISwapChain1> swap_chain) {
        host_ptr->post([host_ptr, visual_ref, swap_chain]() {
            BOSON_HR_CHECK(visual_ref->SetContent(swap_chain.Get()));
            host_ptr->schedule_commit();
        });
    });
}

Surface::~Surface() {
    m_core->close();
    m_device->render_loop().remove(m_core.get());
    if (m_visual) {
        m_visual->SetContent(nullptr);
    }
    m_host.unregister(this);
}

void Surface::set_frame_callback(FrameCallback callback) {
    m_core->set_callback(std::move(callback));
}

void Surface::request_frame() noexcept {
    m_core->request_frame();
}

void Surface::set_bounds(RECT physical_px) {
    m_bounds_dip.reset();
    if (EqualRect(&physical_px, &m_bounds) != FALSE) {
        return;
    }
    m_bounds = physical_px;
    apply_geometry();
}

void Surface::set_bounds_dip(RECT dip) {
    m_bounds_dip = dip;
    const UINT dpi = m_host.window().dpi();
    const auto scale = [dpi](LONG v) { return MulDiv(v, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); };
    m_bounds = RECT{scale(dip.left), scale(dip.top), scale(dip.right), scale(dip.bottom)};
    apply_geometry();
}

void Surface::set_clip(std::optional<RECT> local_px) {
    m_clip = local_px;
    apply_geometry();
}

void Surface::set_visible(bool visible) {
    if (m_visible == visible) {
        return;
    }
    m_visible = visible;
    m_core->set_pause(detail::kPauseHidden, !visible);
    apply_geometry();
}

void Surface::set_opacity(float opacity) {
    m_opacity = std::clamp(opacity, 0.0f, 1.0f);
    apply_geometry();
}

void Surface::set_layer(Layer layer, int z_index) {
    if (layer == m_layer && z_index == m_z_index) {
        return;
    }
    const Layer old = m_layer;
    m_layer = layer;
    m_z_index = z_index;
    if (!m_host.ensure_layer(layer)) {
        m_layer = old;
        return;
    }
    m_host.rebuild_layer(old);
    if (old != layer) {
        m_host.rebuild_layer(layer);
    }
}

void Surface::set_dpi_scale(float scale) {
    m_dpi_scale_override = scale;
    m_core->set_dpi_scale(scale > 0.0f ? scale
                                       : static_cast<float>(m_host.window().dpi()) / static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    m_core->request_frame();
}

SurfaceStats Surface::stats() const {
    return m_core->stats();
}

void Surface::on_window_dpi(UINT dpi) {
    if (m_bounds_dip.has_value()) {
        const RECT dip = *m_bounds_dip;
        const auto scale = [dpi](LONG v) { return MulDiv(v, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); };
        m_bounds = RECT{scale(dip.left), scale(dip.top), scale(dip.right), scale(dip.bottom)};
        apply_geometry();
    }
    if (m_dpi_scale_override <= 0.0f) {
        m_core->set_dpi_scale(static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI));
        m_core->request_frame();
    }
}

void Surface::set_paused(bool paused) noexcept {
    m_core->set_pause(detail::kPauseMinimized, paused);
}

void Surface::apply_geometry() {
    const LONG width = std::max<LONG>(1, m_bounds.right - m_bounds.left);
    const LONG height = std::max<LONG>(1, m_bounds.bottom - m_bounds.top);
    m_core->set_size(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    if (!m_visual) {
        return;
    }
    BOSON_HR_CHECK(m_visual->SetOffsetX(static_cast<float>(m_bounds.left)));
    BOSON_HR_CHECK(m_visual->SetOffsetY(static_cast<float>(m_bounds.top)));

    RECT clip{0, 0, width, height};
    if (m_clip.has_value()) {
        RECT requested = *m_clip;
        if (IntersectRect(&clip, &clip, &requested) == FALSE) {
            clip = RECT{0, 0, 0, 0};
        }
    }
    if (IDCompositionDesktopDevice* dcomp = m_host.dcomp(); dcomp != nullptr) {
        ComPtr<IDCompositionRectangleClip> rect_clip;
        if (BOSON_HR_CHECK(dcomp->CreateRectangleClip(&rect_clip))) {
            rect_clip->SetLeft(static_cast<float>(clip.left));
            rect_clip->SetTop(static_cast<float>(clip.top));
            rect_clip->SetRight(static_cast<float>(clip.right));
            rect_clip->SetBottom(static_cast<float>(clip.bottom));
            BOSON_HR_CHECK(m_visual->SetClip(rect_clip.Get()));
        }
    }
    ComPtr<IDCompositionVisual3> visual3;
    if (SUCCEEDED(m_visual.As(&visual3))) {
        visual3->SetVisible(m_visible ? TRUE : FALSE);
        visual3->SetOpacity(m_opacity);
    }
    m_host.schedule_commit();
}

// ---- SurfaceHost -----------------------------------------------------------------------------------------

SurfaceHost::SurfaceHost(core::Window& window, std::shared_ptr<core::CompositionHost> composition)
    : m_window(window)
    , m_composition(std::move(composition)) {
    m_post_message = RegisterWindowMessageW(L"Boson.Gpu.SurfaceHost.Post");
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
        if (msg == m_post_message && m_post_message != 0) {
            drain_posted();
            return 0;
        }
        return std::nullopt;
    });
    using core::WindowEventKind;
    m_listeners.push_back(m_window.on(WindowEventKind::Minimize, [this](core::WindowEvent&) { on_minimized(true); }));
    m_listeners.push_back(m_window.on(WindowEventKind::Restore, [this](core::WindowEvent&) { on_minimized(false); }));
    m_listeners.push_back(m_window.on(WindowEventKind::Maximize, [this](core::WindowEvent&) { on_minimized(false); }));
    m_listeners.push_back(m_window.on(WindowEventKind::DpiChanged, [this](core::WindowEvent& e) { on_dpi(e.dpi); }));
    m_minimized = m_window.is_minimized();
}

SurfaceHost::SurfaceHost(core::WebViewHost& host)
    : SurfaceHost(host.window(), host.composition()) {}

SurfaceHost::~SurfaceHost() {
    for (const auto id : m_listeners) {
        m_window.off(id);
    }
    if (m_hook != 0) {
        m_window.remove_message_hook(m_hook);
    }
    for (LayerTree* tree : {&m_below, &m_above}) {
        if (tree->root) {
            tree->root->RemoveAllVisuals();
        }
        if (tree->target) {
            tree->target->SetRoot(nullptr);
        }
    }
    commit();
    std::scoped_lock guard(*m_post_mutex);
    m_posted->clear();
}

IDCompositionDesktopDevice* SurfaceHost::dcomp() const noexcept {
    if (m_composition) {
        return m_composition->device();
    }
    return m_device.Get();
}

bool SurfaceHost::ensure_layer(Layer layer) {
    LayerTree& tree = layer == Layer::AboveWebView ? m_above : m_below;
    if (tree.root) {
        return true;
    }
    if (m_composition) {
        tree.root = layer == Layer::AboveWebView ? m_composition->overlay() : m_composition->underlay();
        return tree.root != nullptr;
    }
    if (!m_device && !BOSON_HR_CHECK(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&m_device)))) {
        return false;
    }
    // topmost = FALSE draws behind the WebView child windows, TRUE above them.
    if (!BOSON_HR_CHECK(m_device->CreateTargetForHwnd(m_window.hwnd(), layer == Layer::AboveWebView ? TRUE : FALSE, &tree.target)) ||
        !BOSON_HR_CHECK(m_device->CreateVisual(&tree.root)) || !BOSON_HR_CHECK(tree.target->SetRoot(tree.root.Get()))) {
        tree = LayerTree{};
        return false;
    }
    return true;
}

ComPtr<IDCompositionVisual2> SurfaceHost::create_visual() {
    if (m_composition) {
        return m_composition->create_visual();
    }
    ComPtr<IDCompositionVisual2> visual;
    if (m_device) {
        BOSON_HR_CHECK(m_device->CreateVisual(&visual));
    }
    return visual;
}

std::unique_ptr<Surface> SurfaceHost::create_surface(std::shared_ptr<Device> device, SurfaceOptions options) {
    if (!device || !ensure_layer(options.layer)) {
        return nullptr;
    }
    ComPtr<IDCompositionVisual2> visual = create_visual();
    if (!visual) {
        return nullptr;
    }
    if (options.dpi_scale <= 0.0f) {
        options.dpi_scale = 0.0f;
    }
    auto surface = std::make_unique<Surface>(detail::SurfaceKey{}, *this, device, options, visual, m_next_id++);
    surface->m_order = m_next_order++;
    m_surfaces.push_back(surface.get());
    rebuild_layer(options.layer);
    surface->apply_geometry();
    if (m_minimized) {
        surface->set_paused(true);
    }
    device->render_loop().add(surface->m_core);
    surface->request_frame();
    return surface;
}

Surface* SurfaceHost::find(std::uint32_t id) const noexcept {
    const auto it = std::ranges::find(m_surfaces, id, &Surface::id);
    return it == m_surfaces.end() ? nullptr : *it;
}

void SurfaceHost::rebuild_layer(Layer layer) {
    LayerTree& tree = layer == Layer::AboveWebView ? m_above : m_below;
    if (!tree.root) {
        return;
    }
    std::vector<Surface*> members;
    for (Surface* surface : m_surfaces) {
        if (surface->m_layer == layer) {
            members.push_back(surface);
        }
    }
    std::ranges::sort(members, [](const Surface* a, const Surface* b) {
        return a->m_z_index != b->m_z_index ? a->m_z_index < b->m_z_index : a->m_order < b->m_order;
    });
    tree.root->RemoveAllVisuals();
    IDCompositionVisual* previous = nullptr;
    for (Surface* surface : members) {
        BOSON_HR_CHECK(tree.root->AddVisual(surface->m_visual.Get(), TRUE, previous));
        previous = surface->m_visual.Get();
    }
    schedule_commit();
}

void SurfaceHost::unregister(Surface* surface) noexcept {
    const auto it = std::ranges::find(m_surfaces, surface);
    if (it == m_surfaces.end()) {
        return;
    }
    const Layer layer = surface->m_layer;
    m_surfaces.erase(it);
    LayerTree& tree = layer == Layer::AboveWebView ? m_above : m_below;
    if (tree.root && surface->m_visual) {
        tree.root->RemoveVisual(surface->m_visual.Get());
    }
    schedule_commit();
}

bool SurfaceHost::commit() {
    m_commit_scheduled = false;
    if (m_composition) {
        return m_composition->commit();
    }
    return m_device ? BOSON_HR_CHECK(m_device->Commit()) : true;
}

void SurfaceHost::schedule_commit() {
    if (m_commit_scheduled) {
        return;
    }
    m_commit_scheduled = true;
    post([this]() {
        if (m_commit_scheduled) {
            commit();
        }
    });
}

void SurfaceHost::post(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    {
        std::scoped_lock guard(*m_post_mutex);
        m_posted->push_back(std::move(fn));
    }
    PostMessageW(m_window.hwnd(), m_post_message, 0, 0);
}

void SurfaceHost::drain_posted() {
    std::vector<std::function<void()>> local;
    {
        std::scoped_lock guard(*m_post_mutex);
        local.swap(*m_posted);
    }
    for (auto& fn : local) {
        fn();
    }
}

void SurfaceHost::on_minimized(bool minimized) {
    if (m_minimized == minimized) {
        return;
    }
    m_minimized = minimized;
    for (Surface* surface : m_surfaces) {
        surface->set_paused(minimized);
    }
}

void SurfaceHost::on_dpi(UINT dpi) {
    for (Surface* surface : m_surfaces) {
        surface->on_window_dpi(dpi);
    }
}

} // namespace boson::gpu
