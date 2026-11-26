#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::gpu {

class RenderLoop;

/// Graphics API a Device drives. Both present through the same DXGI composition swap chains (gpu::Surface).
enum class Backend : std::uint8_t {
    D3D11,
    /// ID3D12Device + direct queue. The built-in Renderer2D draws through D3D11On12 on the same queue.
    D3D12,
};

enum class AdapterPreference : std::uint8_t {
    Default,         ///< DXGI_GPU_PREFERENCE_UNSPECIFIED (what the user / Windows graphics settings chose)
    HighPerformance, ///< discrete GPU on hybrid laptops
    MinimumPower,    ///< integrated GPU on hybrid laptops
    Warp,            ///< Microsoft Basic Render Driver (software); always available
};

struct DeviceOptions {
    Backend backend = Backend::D3D11;
    AdapterPreference adapter = AdapterPreference::HighPerformance;
    /// Exact adapter (AdapterInfo::luid); overrides adapter when it matches an installed adapter.
    std::optional<std::uint64_t> adapter_luid;
    /// D3D11 / D3D12 debug layer (needs the Graphics Tools optional feature; ignored when missing).
    bool debug_layer = false;
    /// Fall back to WARP when no hardware adapter can create a device.
    bool allow_warp_fallback = true;
};

struct AdapterInfo {
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t dedicated_video_memory = 0; ///< bytes
    std::uint64_t shared_system_memory = 0;   ///< bytes
    std::uint64_t luid = 0;                   ///< LUID as (HighPart << 32) | LowPart
    bool software = false;
};

struct DeviceInfo {
    std::string backend;       ///< "d3d11" | "d3d12"
    AdapterInfo adapter;
    std::string feature_level; ///< "12_1", "11_0", ...
    std::uint32_t feature_level_value = 0; ///< D3D_FEATURE_LEVEL
    bool tearing_supported = false;        ///< DXGI_FEATURE_PRESENT_ALLOW_TEARING
    bool compositor_clock = false;         ///< DCompositionWaitForCompositorClock exported by dcomp.dll
    std::uint64_t generation = 0;          ///< +1 after every device-lost recovery
};

struct DeviceLostInfo {
    HRESULT reason = S_OK;     ///< GetDeviceRemovedReason()
    bool recovered = false;    ///< a new device was created; resources are re-created lazily
    std::uint64_t generation = 0;
};

/// @brief Implemented by objects holding device-dependent resources (Renderer2D, GlyphAtlas, VulkanInterop,
/// surfaces). release_device_resources() runs with the device lock held, right before a lost device is
/// replaced: drop every COM reference to the old device there (a D3D12 device is a per-adapter singleton and
/// is only re-created once nothing references the removed one) and rebuild lazily on next use.
class DeviceResourceOwner {
public:
    virtual void release_device_resources() noexcept = 0;

protected:
    DeviceResourceOwner() = default;
    ~DeviceResourceOwner() = default;
    DeviceResourceOwner(const DeviceResourceOwner&) = default;
    DeviceResourceOwner& operator=(const DeviceResourceOwner&) = default;
};

[[nodiscard]] std::string_view to_string(Backend backend) noexcept;
[[nodiscard]] std::string feature_level_name(D3D_FEATURE_LEVEL level);
[[nodiscard]] std::string hresult_name(HRESULT hr);

/// @brief One D3D11 or D3D12 device on a chosen adapter, shared by every Surface, Renderer2D and VulkanInterop
/// created from it. Owns the render thread (started lazily by the first Surface).
///
/// Threading: the immediate context (and the D3D12 queue) is single-threaded. Everything that records GPU
/// work holds lock() - the render thread does so around every frame callback. Device-lost is detected without
/// polling (ID3D11Device4::RegisterDeviceRemovedEvent / an ID3D12Fence completion event at UINT64_MAX, both
/// waited on by the render thread, plus DXGI_ERROR_DEVICE_REMOVED from Present) and recovered in place.
class Device : public std::enable_shared_from_this<Device> {
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    using ListenerId = std::uint64_t;
    using DeviceLostCallback = std::function<void(const DeviceLostInfo& info)>;

    /// @brief Creates the device; nullptr when no adapter (and no WARP fallback) can create one.
    [[nodiscard]] static std::shared_ptr<Device> create(DeviceOptions options = {});
    /// @brief Every DXGI adapter, in DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE order.
    [[nodiscard]] static std::vector<AdapterInfo> enumerate_adapters();

    Device(PassKey, DeviceOptions options);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] Backend backend() const noexcept { return m_options.backend; }
    [[nodiscard]] DeviceInfo info() const;
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation.load(std::memory_order_acquire); }
    [[nodiscard]] bool tearing_supported() const noexcept { return m_tearing; }

    /// @brief Serializes GPU work recording (immediate context, D3D12 queue, D3D11On12). Recursive.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> lock() const { return std::unique_lock{m_mutex}; }

    // Valid until the next device-lost recovery (check generation()). D3D12 backend: d3d11() is the D3D11On12
    // device on the D3D12 queue.
    [[nodiscard]] ID3D11Device* d3d11() const noexcept { return m_d3d11.Get(); }
    [[nodiscard]] ID3D11DeviceContext* d3d11_context() const noexcept { return m_context.Get(); }
    [[nodiscard]] ID3D11DeviceContext4* d3d11_context4() const noexcept { return m_context4.Get(); }
    [[nodiscard]] ID3D12Device* d3d12() const noexcept { return m_d3d12.Get(); }
    [[nodiscard]] ID3D12CommandQueue* d3d12_queue() const noexcept { return m_queue.Get(); }
    [[nodiscard]] ID3D11On12Device* d3d11on12() const noexcept { return m_d3d11on12.Get(); }
    [[nodiscard]] IDXGIFactory2* dxgi_factory() const noexcept { return m_factory.Get(); }
    [[nodiscard]] IDXGIAdapter1* adapter() const noexcept { return m_adapter.Get(); }
    /// @brief The object swap chains are created against (D3D11 device or D3D12 queue).
    [[nodiscard]] IUnknown* swap_chain_device() const noexcept;

    /// @brief S_OK while the device is healthy, else GetDeviceRemovedReason().
    [[nodiscard]] HRESULT removed_reason() const noexcept;
    /// @brief Waits for the GPU to finish all submitted work (flush + fence). Holds lock().
    void wait_idle();

    /// @brief Reports a device-lost HRESULT seen by a caller (e.g. Present). Recovers when the device really is
    /// removed: releases every DeviceResourceOwner, re-creates the device on the same adapter preference,
    /// bumps generation() and calls the device-lost listeners (on the calling thread). Returns true when the
    /// device is usable afterwards.
    bool handle_device_lost(HRESULT hr);
    /// @brief true while a lost device could not be re-created yet (the render thread retries every 50 ms).
    [[nodiscard]] bool recovery_pending() const noexcept { return m_recovery_pending.load(std::memory_order_acquire); }
    /// @brief Retries a pending recovery (render thread). Returns true when the device is usable.
    bool retry_recovery();
    /// @brief Test hook: forces the recovery path (ID3D12Device5::RemoveDevice on D3D12; D3D11 has no such
    /// API, so a D3D11 device is re-created as if it had been removed).
    bool simulate_device_lost();

    ListenerId on_device_lost(DeviceLostCallback callback);
    void off_device_lost(ListenerId id) noexcept;

    void add_resource_owner(DeviceResourceOwner* owner);
    void remove_resource_owner(DeviceResourceOwner* owner) noexcept;

    /// @brief The render thread every Surface of this device is paced and drawn on (started on first use).
    [[nodiscard]] RenderLoop& render_loop();
    /// @brief Handle signaled when the device is removed (waited on by the render thread).
    [[nodiscard]] HANDLE removed_event() const noexcept { return m_removed_event; }

private:
    bool initialize();
    bool create_devices();
    bool create_d3d11(IDXGIAdapter1* adapter, bool warp);
    bool create_d3d12(IDXGIAdapter1* adapter);
    bool recover_locked(HRESULT reason, DeviceLostInfo& out);
    void watch_removal();
    void release_devices() noexcept;
    void notify_lost(const DeviceLostInfo& info);

    DeviceOptions m_options;
    mutable std::recursive_mutex m_mutex;
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<bool> m_recovery_pending{false};
    HRESULT m_lost_reason = S_OK;
    bool m_tearing = false;
    bool m_compositor_clock = false;
    D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;
    AdapterInfo m_adapter_info;

    Microsoft::WRL::ComPtr<IDXGIFactory2> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3d11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    Microsoft::WRL::ComPtr<ID3D11On12Device> m_d3d11on12;
    Microsoft::WRL::ComPtr<ID3D12Device> m_d3d12;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_removal_fence; ///< D3D12: SetEventOnCompletion(UINT64_MAX) fires on removal
    Microsoft::WRL::ComPtr<ID3D12Fence> m_idle_fence;
    std::uint64_t m_idle_value = 0;
    DWORD m_removed_cookie = 0;
    bool m_removed_registered = false;
    bool m_simulated_loss = false;
    bool m_singleton_busy = false;
    HANDLE m_removed_event = nullptr;
    HANDLE m_idle_event = nullptr;

    struct LostListener {
        ListenerId id = 0;
        DeviceLostCallback fn;
    };
    std::mutex m_listener_mutex;
    std::vector<LostListener> m_listeners;
    ListenerId m_next_listener = 1;
    std::vector<DeviceResourceOwner*> m_owners; ///< guarded by m_mutex

    std::once_flag m_loop_once;
    std::unique_ptr<RenderLoop> m_loop;
};

} // namespace boson::gpu
