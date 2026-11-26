#pragma once

#include <windows.h>
#include <wrl.h>
#include <dcomp.h>

#include <memory>
#include <vector>

namespace boson::core {

class WebView;

/// @brief One DirectComposition device + HWND target + root visual shared by every composition-hosted
/// WebView in a window. Each WebView gets its own child visual; z-order, offset, opacity and (rounded) clip
/// are applied to that visual. UI thread only.
class CompositionHost {
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    /// @brief Creates the device and target for hwnd; returns nullptr when DirectComposition is unavailable.
    [[nodiscard]] static std::shared_ptr<CompositionHost> create(HWND hwnd);

    CompositionHost(PassKey, HWND hwnd);
    ~CompositionHost();

    CompositionHost(const CompositionHost&) = delete;
    CompositionHost& operator=(const CompositionHost&) = delete;
    CompositionHost(CompositionHost&&) = delete;
    CompositionHost& operator=(CompositionHost&&) = delete;

    [[nodiscard]] Microsoft::WRL::ComPtr<IDCompositionVisual2> create_visual() const;

    /// @brief Adds a WebView's visual at the top of the z-order.
    void attach(WebView* owner, IDCompositionVisual2* visual);
    void detach(WebView* owner) noexcept;
    void raise(WebView* owner);
    void lower(WebView* owner);

    bool set_offset(IDCompositionVisual2* visual, int x, int y) const noexcept;
    bool set_opacity(IDCompositionVisual2* visual, float opacity) const noexcept;
    /// @brief Clips the visual to {0,0,width,height}, with rounded corners when radius > 0.
    bool set_clip(IDCompositionVisual2* visual, int width, int height, float radius) const noexcept;
    bool set_visible(IDCompositionVisual2* visual, bool visible) const noexcept;
    bool commit() const noexcept;

    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }
    [[nodiscard]] IDCompositionDesktopDevice* device() const noexcept { return m_device.Get(); }
    /// @brief Container visuals kept below / above every WebView visual (native GPU surfaces, gpu::SurfaceHost).
    [[nodiscard]] IDCompositionVisual2* underlay() const noexcept { return m_underlay.Get(); }
    [[nodiscard]] IDCompositionVisual2* overlay() const noexcept { return m_overlay.Get(); }

    /// @brief Calls fn(WebView*) from the top-most visual down; stops when fn returns true.
    template <typename Fn>
    WebView* find_top_down(Fn&& fn) const {
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            if (fn(it->owner)) {
                return it->owner;
            }
        }
        return nullptr;
    }

private:
    bool initialize();
    void rebuild_tree() noexcept;

    struct Entry {
        WebView* owner = nullptr;
        Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    };

    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> m_device;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_target;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> m_root;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> m_underlay;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> m_overlay;
    std::vector<Entry> m_entries; // bottom -> top
};

} // namespace boson::core
