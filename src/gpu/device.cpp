#include <boson/gpu/device.hpp>
#include <boson/gpu/render_loop.hpp>

#include <boson/core/result.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>

namespace boson::gpu {

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::array<D3D_FEATURE_LEVEL, 6> kLevels = {
    D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
};

std::uint64_t luid_value(LUID luid) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}

LUID to_luid(std::uint64_t value) noexcept {
    LUID luid{};
    luid.LowPart = static_cast<DWORD>(value & 0xFFFFFFFFu);
    luid.HighPart = static_cast<LONG>(static_cast<std::uint32_t>(value >> 32));
    return luid;
}

AdapterInfo describe(IDXGIAdapter1* adapter) {
    AdapterInfo info;
    DXGI_ADAPTER_DESC1 desc{};
    if (adapter == nullptr || FAILED(adapter->GetDesc1(&desc))) {
        return info;
    }
    info.name = core::wide_to_utf8(desc.Description);
    info.vendor_id = desc.VendorId;
    info.device_id = desc.DeviceId;
    info.dedicated_video_memory = desc.DedicatedVideoMemory;
    info.shared_system_memory = desc.SharedSystemMemory;
    info.luid = luid_value(desc.AdapterLuid);
    info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    return info;
}

bool is_device_lost(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

ComPtr<IDXGIFactory2> create_factory(bool debug) {
    ComPtr<IDXGIFactory2> factory;
    if (debug && SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)))) {
        return factory;
    }
    BOSON_HR_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    return factory;
}

/// Hardware adapters in the requested order (software adapters skipped).
std::vector<ComPtr<IDXGIAdapter1>> ordered_adapters(IDXGIFactory2* factory, AdapterPreference preference) {
    std::vector<ComPtr<IDXGIAdapter1>> out;
    if (factory == nullptr) {
        return out;
    }
    ComPtr<IDXGIFactory6> factory6;
    const DXGI_GPU_PREFERENCE pref = preference == AdapterPreference::HighPerformance ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                     : preference == AdapterPreference::MinimumPower  ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                                                      : DXGI_GPU_PREFERENCE_UNSPECIFIED;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory6->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (adapter && !describe(adapter.Get()).software) {
                out.push_back(adapter);
            }
        }
        return out;
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (adapter && !describe(adapter.Get()).software) {
            out.push_back(adapter);
        }
    }
    return out;
}

} // namespace

std::string_view to_string(Backend backend) noexcept {
    return backend == Backend::D3D12 ? "d3d12" : "d3d11";
}

std::string feature_level_name(D3D_FEATURE_LEVEL level) {
    const auto value = static_cast<unsigned>(level);
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%u_%u", (value >> 12) & 0xFu, (value >> 8) & 0xFu);
    return buffer;
}

std::string hresult_name(HRESULT hr) {
    switch (hr) {
    case S_OK: return "S_OK";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    default: break;
    }
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

std::shared_ptr<Device> Device::create(DeviceOptions options) {
    auto device = std::make_shared<Device>(PassKey{}, options);
    if (!device->initialize()) {
        return nullptr;
    }
    return device;
}

std::vector<AdapterInfo> Device::enumerate_adapters() {
    std::vector<AdapterInfo> out;
    const ComPtr<IDXGIFactory2> factory = create_factory(false);
    for (const auto& adapter : ordered_adapters(factory.Get(), AdapterPreference::HighPerformance)) {
        out.push_back(describe(adapter.Get()));
    }
    ComPtr<IDXGIFactory4> factory4;
    ComPtr<IDXGIAdapter1> warp;
    if (factory && SUCCEEDED(factory.As(&factory4)) && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        out.push_back(describe(warp.Get()));
    }
    return out;
}

Device::Device(PassKey, DeviceOptions options)
    : m_options(options) {}

Device::~Device() {
    m_loop.reset(); // joins the render thread before any device object goes away
    const auto guard = lock();
    release_devices();
    if (m_removed_event != nullptr) {
        CloseHandle(m_removed_event);
    }
    if (m_idle_event != nullptr) {
        CloseHandle(m_idle_event);
    }
}

bool Device::initialize() {
    m_removed_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_idle_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_removed_event == nullptr || m_idle_event == nullptr) {
        return false;
    }
    if (HMODULE dcomp = GetModuleHandleW(L"dcomp.dll"); dcomp != nullptr) {
        m_compositor_clock = GetProcAddress(dcomp, "DCompositionWaitForCompositorClock") != nullptr;
    }
    const auto guard = lock();
    return create_devices();
}

bool Device::create_devices() {
    m_singleton_busy = false;
    m_factory = create_factory(m_options.debug_layer);
    if (!m_factory) {
        return false;
    }
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(m_factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        m_tearing = tearing != FALSE;
    }

    const auto try_adapter = [this](IDXGIAdapter1* adapter, bool warp) {
        const bool ok = m_options.backend == Backend::D3D12 ? create_d3d12(adapter) : create_d3d11(adapter, warp);
        if (ok) {
            m_adapter = adapter;
            m_adapter_info = describe(adapter);
        }
        return ok;
    };

    ComPtr<IDXGIFactory4> factory4;
    const bool have_factory4 = SUCCEEDED(m_factory.As(&factory4));
    bool created = false;
    if (m_options.adapter == AdapterPreference::Warp) {
        ComPtr<IDXGIAdapter1> warp;
        created = have_factory4 && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && try_adapter(warp.Get(), true);
    } else {
        if (m_options.adapter_luid.has_value() && have_factory4) {
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(factory4->EnumAdapterByLuid(to_luid(*m_options.adapter_luid), IID_PPV_ARGS(&adapter)))) {
                created = try_adapter(adapter.Get(), describe(adapter.Get()).software);
            }
        }
        if (!created && !m_singleton_busy) {
            for (const auto& adapter : ordered_adapters(m_factory.Get(), m_options.adapter)) {
                if (try_adapter(adapter.Get(), false)) {
                    created = true;
                    break;
                }
                if (m_singleton_busy) {
                    break; // retry on the same adapter instead of silently moving to another one
                }
            }
        }
        if (!created && !m_singleton_busy && m_options.allow_warp_fallback && have_factory4) {
            ComPtr<IDXGIAdapter1> warp;
            created = SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && try_adapter(warp.Get(), true);
        }
    }
    if (!created) {
        if (!m_singleton_busy) {
            core::hr_check(E_FAIL, "gpu::Device: no adapter could create a device");
        }
        release_devices();
        return false;
    }
    watch_removal();
    return true;
}

bool Device::create_d3d11(IDXGIAdapter1* adapter, bool warp) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (m_options.debug_layer) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    (void)warp; // WARP is just another DXGI adapter here (EnumWarpAdapter)
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels.data(),
                                   static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION, &device, &level, &context);
    if (hr == E_INVALIDARG) { // pre-11.1 runtimes reject the 12_x / 11_1 levels
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels.data() + 3, 3, D3D11_SDK_VERSION,
                               &device, &level, &context);
    }
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) { // debug layer not installed
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels.data(),
                               static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION, &device, &level, &context);
    }
    if (!BOSON_HR_CHECK(hr)) {
        return false;
    }
    m_d3d11 = device;
    m_context = context;
    m_context.As(&m_context4);
    m_feature_level = level;
    return true;
}

bool Device::create_d3d12(IDXGIAdapter1* adapter) {
    if (m_options.debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }
    ComPtr<ID3D12Device> device;
    if (!BOSON_HR_CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        return false;
    }
    if (device->GetDeviceRemovedReason() != S_OK) {
        m_singleton_busy = true; // the removed singleton is still referenced somewhere; retried later
        return false;
    }
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
    levels.NumFeatureLevels = static_cast<UINT>(kLevels.size());
    levels.pFeatureLevelsRequested = kLevels.data();
    m_feature_level = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)))
                          ? levels.MaxSupportedFeatureLevel
                          : D3D_FEATURE_LEVEL_11_0;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!BOSON_HR_CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (m_options.debug_layer) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    IUnknown* queues[] = {queue.Get()};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    HRESULT hr = D3D11On12CreateDevice(device.Get(), flags, nullptr, 0, queues, 1, 0, &device11, &context11, nullptr);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        hr = D3D11On12CreateDevice(device.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &device11,
                                   &context11, nullptr);
    }
    if (!BOSON_HR_CHECK(hr)) {
        return false;
    }
    ComPtr<ID3D11On12Device> on12;
    if (!BOSON_HR_CHECK(device11.As(&on12))) {
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    if (!BOSON_HR_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return false;
    }
    m_d3d12 = device;
    m_queue = queue;
    m_d3d11 = device11;
    m_context = context11;
    m_context.As(&m_context4);
    m_d3d11on12 = on12;
    m_idle_fence = fence;
    m_idle_value = 0;
    return true;
}

void Device::watch_removal() {
    ResetEvent(m_removed_event);
    if (m_d3d12) {
        if (BOSON_HR_CHECK(m_d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_removal_fence)))) {
            // A removed device signals every fence to UINT64_MAX, so this event doubles as a removal notification.
            BOSON_HR_CHECK(m_removal_fence->SetEventOnCompletion(UINT64_MAX, m_removed_event));
        }
        return;
    }
    ComPtr<ID3D11Device4> device4;
    if (m_d3d11 && SUCCEEDED(m_d3d11.As(&device4)) &&
        SUCCEEDED(device4->RegisterDeviceRemovedEvent(m_removed_event, &m_removed_cookie))) {
        m_removed_registered = true;
    }
}

void Device::release_devices() noexcept {
    if (m_removed_registered && m_d3d11) {
        ComPtr<ID3D11Device4> device4;
        if (SUCCEEDED(m_d3d11.As(&device4))) {
            device4->UnregisterDeviceRemoved(m_removed_cookie);
        }
    }
    m_removed_registered = false;
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_removal_fence.Reset();
    m_idle_fence.Reset();
    m_context4.Reset();
    m_context.Reset();
    m_d3d11on12.Reset();
    m_d3d11.Reset();
    m_queue.Reset();
    m_d3d12.Reset();
    m_adapter.Reset();
    m_factory.Reset();
}

DeviceInfo Device::info() const {
    const auto guard = lock();
    DeviceInfo out;
    out.backend = std::string(to_string(m_options.backend));
    out.adapter = m_adapter_info;
    out.feature_level = feature_level_name(m_feature_level);
    out.feature_level_value = static_cast<std::uint32_t>(m_feature_level);
    out.tearing_supported = m_tearing;
    out.compositor_clock = m_compositor_clock;
    out.generation = generation();
    return out;
}

IUnknown* Device::swap_chain_device() const noexcept {
    if (m_options.backend == Backend::D3D12) {
        return m_queue.Get();
    }
    return m_d3d11.Get();
}

HRESULT Device::removed_reason() const noexcept {
    if (m_simulated_loss) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (m_d3d12) {
        return m_d3d12->GetDeviceRemovedReason();
    }
    if (m_d3d11) {
        return m_d3d11->GetDeviceRemovedReason();
    }
    return DXGI_ERROR_DEVICE_REMOVED;
}

void Device::wait_idle() {
    const auto guard = lock();
    if (!m_context) {
        return;
    }
    m_context->Flush();
    if (m_queue && m_idle_fence) {
        const std::uint64_t value = ++m_idle_value;
        if (SUCCEEDED(m_queue->Signal(m_idle_fence.Get(), value)) && m_idle_fence->GetCompletedValue() < value &&
            SUCCEEDED(m_idle_fence->SetEventOnCompletion(value, m_idle_event))) {
            WaitForSingleObject(m_idle_event, 5000);
        }
        return;
    }
    D3D11_QUERY_DESC desc{};
    desc.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> query;
    if (!m_d3d11 || FAILED(m_d3d11->CreateQuery(&desc, &query))) {
        return;
    }
    m_context->End(query.Get());
    BOOL done = FALSE;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (m_context->GetData(query.Get(), &done, sizeof(done), 0) == S_FALSE && GetTickCount64() < deadline) {
        SwitchToThread();
    }
}

bool Device::recover_locked(HRESULT reason, DeviceLostInfo& out) {
    ResetEvent(m_removed_event); // re-armed by watch_removal() on the new device
    if (!m_recovery_pending.load(std::memory_order_acquire)) {
        m_lost_reason = reason;
        // Every owner drops its references first: a D3D12 device is a per-adapter singleton.
        const std::vector<DeviceResourceOwner*> owners = m_owners;
        for (DeviceResourceOwner* owner : owners) {
            owner->release_device_resources();
        }
    }
    release_devices();
    m_simulated_loss = false;
    const bool ok = create_devices();
    m_recovery_pending.store(!ok, std::memory_order_release);
    out.reason = m_lost_reason;
    out.recovered = ok;
    if (ok) {
        m_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    out.generation = generation();
    return ok;
}

bool Device::handle_device_lost(HRESULT hr) {
    DeviceLostInfo info;
    bool ok = false;
    {
        const auto guard = lock();
        if (m_recovery_pending.load(std::memory_order_acquire)) {
            return false;
        }
        const HRESULT reason = removed_reason();
        if (reason == S_OK && !is_device_lost(hr)) {
            return true;
        }
        if (reason == S_OK) {
            return true; // a stale error from before the last recovery
        }
        ok = recover_locked(reason, info);
    }
    notify_lost(info);
    if (m_loop) {
        m_loop->wake();
    }
    return ok;
}

bool Device::retry_recovery() {
    DeviceLostInfo info;
    bool ok = false;
    {
        const auto guard = lock();
        if (!m_recovery_pending.load(std::memory_order_acquire)) {
            return true;
        }
        ok = recover_locked(m_lost_reason, info);
    }
    if (ok) {
        notify_lost(info);
    }
    return ok;
}

bool Device::simulate_device_lost() {
    {
        const auto guard = lock();
        if (m_d3d12) {
            ComPtr<ID3D12Device5> device5;
            if (SUCCEEDED(m_d3d12.As(&device5))) {
                device5->RemoveDevice(); // the removal fence fires; the render thread recovers
            } else {
                m_simulated_loss = true;
            }
        } else {
            m_simulated_loss = true;
            SetEvent(m_removed_event);
        }
    }
    if (m_loop) {
        m_loop->wake();
        return true;
    }
    return handle_device_lost(DXGI_ERROR_DEVICE_REMOVED);
}

void Device::notify_lost(const DeviceLostInfo& info) {
    std::vector<DeviceLostCallback> callbacks;
    {
        std::scoped_lock guard(m_listener_mutex);
        for (const auto& listener : m_listeners) {
            callbacks.push_back(listener.fn);
        }
    }
    for (const auto& callback : callbacks) {
        if (callback) {
            callback(info);
        }
    }
}

Device::ListenerId Device::on_device_lost(DeviceLostCallback callback) {
    std::scoped_lock guard(m_listener_mutex);
    const ListenerId id = m_next_listener++;
    m_listeners.push_back(LostListener{.id = id, .fn = std::move(callback)});
    return id;
}

void Device::off_device_lost(ListenerId id) noexcept {
    std::scoped_lock guard(m_listener_mutex);
    std::erase_if(m_listeners, [id](const LostListener& l) { return l.id == id; });
}

void Device::add_resource_owner(DeviceResourceOwner* owner) {
    const auto guard = lock();
    m_owners.push_back(owner);
}

void Device::remove_resource_owner(DeviceResourceOwner* owner) noexcept {
    const auto guard = lock();
    std::erase(m_owners, owner);
}

RenderLoop& Device::render_loop() {
    std::call_once(m_loop_once, [this]() { m_loop = std::make_unique<RenderLoop>(*this); });
    return *m_loop;
}

} // namespace boson::gpu
