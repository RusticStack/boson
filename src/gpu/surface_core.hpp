#pragma once

#include <boson/gpu/surface.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace boson::gpu::detail {

/// Pause reasons (bit mask): rendering is skipped while any bit is set.
inline constexpr std::uint32_t kPauseMinimized = 1u << 0;
inline constexpr std::uint32_t kPauseHidden = 1u << 1;

/// @brief The render-thread half of a Surface: swap chain, waitable, back buffer views and frame state.
/// Swap chains are created / resized only on the render thread, with the device lock held.
class SurfaceCore final : public DeviceResourceOwner {
public:
    using SetContentFn = std::function<void(Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain)>;

    SurfaceCore(Device& device, const SurfaceOptions& options, Surface* owner);
    ~SurfaceCore();

    SurfaceCore(const SurfaceCore&) = delete;
    SurfaceCore& operator=(const SurfaceCore&) = delete;
    SurfaceCore(SurfaceCore&&) = delete;
    SurfaceCore& operator=(SurfaceCore&&) = delete;

    // ---- Any thread -------------------------------------------------------------------------------------
    void request_frame() noexcept;
    void set_size(std::uint32_t width, std::uint32_t height) noexcept;
    void set_pause(std::uint32_t bit, bool on) noexcept;
    void set_dpi_scale(float scale) noexcept { m_dpi_scale.store(scale, std::memory_order_relaxed); }
    void set_callback(Surface::FrameCallback callback);
    void set_content_fn(SetContentFn fn);
    /// @brief Stops all callbacks; waits for a running frame callback of this surface to return.
    void close();
    [[nodiscard]] SurfaceStats stats() const;
    [[nodiscard]] bool closing() const noexcept { return m_closing.load(std::memory_order_acquire); }

    // ---- Render thread (device lock held) ---------------------------------------------------------------
    /// @brief true when the surface wants a frame and is not paused.
    [[nodiscard]] bool wants_frame() const noexcept;
    /// @brief true when render() can run without waiting (needs a swap chain / resize, or holds a buffer).
    [[nodiscard]] bool ready_without_wait() const noexcept;
    [[nodiscard]] HANDLE waitable() const noexcept { return m_waitable; }
    /// @brief Takes the frame slot when the waitable is signaled (timeout 0) or was signaled by the loop.
    bool try_acquire(bool already_signaled) noexcept;
    [[nodiscard]] bool occluded() const noexcept { return m_occluded; }
    /// @brief Probes an occluded swap chain with DXGI_PRESENT_TEST; clears occluded() when it is visible again.
    void probe_occlusion() noexcept;
    void close_retired() noexcept;
    /// @brief Renders and presents one frame if the slot is held. Returns the Present HRESULT (S_OK if skipped).
    HRESULT render();

    void release_device_resources() noexcept override;

private:
    bool ensure_swap_chain();
    bool resize_if_needed();
    void release_views() noexcept;

    Device& m_device;
    Surface* m_owner = nullptr; ///< guarded by m_callback_mutex
    SurfaceOptions m_options;

    std::atomic<bool> m_dirty{false};
    std::atomic<bool> m_closing{false};
    std::atomic<std::uint32_t> m_pause{0};
    std::atomic<std::uint64_t> m_requested_size{0}; ///< (width << 32) | height
    std::atomic<float> m_dpi_scale{1.0f};
    std::atomic<std::uint64_t> m_requests{0};

    mutable std::mutex m_callback_mutex;
    Surface::FrameCallback m_callback;
    SetContentFn m_set_content;

    // Render-thread state.
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap_chain;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swap_chain3;
    HANDLE m_waitable = nullptr;
    std::array<HANDLE, 4> m_retired{};
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint64_t m_generation = ~0ull;
    UINT m_flags = 0;
    bool m_slot = false;
    bool m_occluded = false;
    bool m_tearing_ok = true;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_back_buffer;          ///< D3D11 backend: buffer 0
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> m_d3d12_buffers;   ///< D3D12 backend
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> m_wrapped;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 4> m_wrapped_rtv;

    LARGE_INTEGER m_qpc_frequency{};
    LARGE_INTEGER m_created{};
    LARGE_INTEGER m_last_present{};
    std::uint64_t m_frame_index = 0;

    mutable std::mutex m_stats_mutex;
    SurfaceStats m_stats;
};

} // namespace boson::gpu::detail
