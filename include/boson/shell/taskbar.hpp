#pragma once

#include <windows.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <boson/core/window.hpp>
#include <boson/shell/icon.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::shell {

enum class TaskbarProgressState {
    None,          ///< no progress bar
    Indeterminate, ///< marquee
    Normal,        ///< green
    Error,         ///< red
    Paused,        ///< yellow
};

struct FlashOptions {
    /// Number of flashes; 0 flashes until stop_flash() (or, with until_foreground, until activated).
    std::uint32_t count = 3;
    /// Keep flashing until the window comes to the foreground (FLASHW_TIMERNOFG).
    bool until_foreground = false;
    bool caption = true;
    bool tray = true;
    /// 0 = the system cursor blink rate.
    std::chrono::milliseconds interval{0};
};

/// @brief FlashWindowEx. Any thread; no-op for a null hwnd.
bool flash_window(HWND hwnd, const FlashOptions& options = {}) noexcept;
bool stop_flash_window(HWND hwnd) noexcept;

struct ThumbnailButton {
    std::uint32_t id = 0; ///< reported to the click callback
    std::shared_ptr<const Icon> icon;
    std::wstring tooltip;
    bool enabled = true;
    bool hidden = false;
    bool dismiss_on_click = false; ///< close the thumbnail preview when clicked
    bool no_background = false;
    bool non_interactive = false; ///< drawn, but no press state (use for status glyphs)
};

/// @brief Taskbar button of one Window (ITaskbarList3/ITaskbarList4): progress, overlay icon, flashing,
/// thumbnail toolbar, thumbnail tooltip and clip. Every member must be called on the window's UI thread.
///
/// The state is cached, so calls made before the taskbar button exists, and every setting after Explorer
/// restarts, are (re)applied when the shell sends the "TaskbarButtonCreated" message.
/// Must not outlive the Window.
class Taskbar {
public:
    static constexpr std::size_t kMaxThumbnailButtons = 7;
    using ThumbnailClickFn = std::function<void(std::uint32_t id)>;

    explicit Taskbar(core::Window& window);
    ~Taskbar();

    Taskbar(const Taskbar&) = delete;
    Taskbar& operator=(const Taskbar&) = delete;
    Taskbar(Taskbar&&) = delete;
    Taskbar& operator=(Taskbar&&) = delete;

    /// @brief true once ITaskbarList3 was created and initialised.
    [[nodiscard]] bool is_available() const noexcept { return m_taskbar != nullptr; }

    bool set_progress_state(TaskbarProgressState state);
    /// @brief fraction in [0, 1]. Switches a None/Indeterminate bar to Normal.
    bool set_progress(double fraction);
    bool set_progress(std::uint64_t completed, std::uint64_t total);
    bool clear_progress() { return set_progress_state(TaskbarProgressState::None); }
    [[nodiscard]] TaskbarProgressState progress_state() const noexcept { return m_progress_state; }
    [[nodiscard]] double progress_value() const noexcept { return m_progress_value; }

    /// @brief Overlay (badge) icon on the taskbar button. description is read by screen readers.
    bool set_overlay_icon(std::shared_ptr<const Icon> icon, std::wstring_view description);
    bool clear_overlay_icon();
    [[nodiscard]] bool has_overlay_icon() const noexcept { return m_overlay != nullptr; }

    bool flash(const FlashOptions& options = {}) const noexcept;
    bool stop_flash() const noexcept;

    /// @brief Replaces the thumbnail toolbar (at most kMaxThumbnailButtons, unique ids). The shell only
    /// lets a window add buttons once, so Boson adds kMaxThumbnailButtons slots and hides the unused ones;
    /// an empty vector hides the toolbar.
    bool set_thumbnail_buttons(std::vector<ThumbnailButton> buttons);
    /// @brief Updates one button (matched by id) in place.
    bool update_thumbnail_button(const ThumbnailButton& button);
    [[nodiscard]] const std::vector<ThumbnailButton>& thumbnail_buttons() const noexcept { return m_buttons; }
    void set_thumbnail_click_callback(ThumbnailClickFn callback) { m_on_click = std::move(callback); }

    /// @brief Tooltip of the thumbnail preview; empty restores the window title.
    bool set_thumbnail_tooltip(std::wstring_view tooltip);
    /// @brief Shows only this client-area rect (client px) in the thumbnail; std::nullopt shows all.
    bool set_thumbnail_clip(std::optional<RECT> client_px);

    /// @brief Number of times "TaskbarButtonCreated" was handled (Explorer start/restart).
    [[nodiscard]] std::uint32_t button_created_count() const noexcept { return m_created_count; }
    /// @brief RegisterWindowMessageW(L"TaskbarButtonCreated").
    [[nodiscard]] static UINT taskbar_button_created_message() noexcept;
    /// @brief Command id the shell sends in WM_COMMAND (with THBN_CLICKED) for thumbnail slot index.
    [[nodiscard]] static constexpr UINT slot_command_id(std::size_t slot) noexcept {
        return kSlotBaseId + static_cast<UINT>(slot);
    }

private:
    static constexpr UINT kSlotBaseId = 0xB050;

    bool ensure_taskbar();
    void reinitialize();
    bool apply_progress();
    bool apply_overlay();
    bool apply_buttons();
    bool apply_tooltip();
    bool apply_clip();
    [[nodiscard]] std::array<THUMBBUTTON, kMaxThumbnailButtons> build_slots() const;
    std::optional<LRESULT> on_message(UINT msg, WPARAM w_param, LPARAM l_param);

    core::Window& m_window;
    core::Window::ListenerId m_hook = 0;
    Microsoft::WRL::ComPtr<ITaskbarList3> m_taskbar;
    bool m_buttons_added = false;
    std::uint32_t m_created_count = 0;

    TaskbarProgressState m_progress_state = TaskbarProgressState::None;
    double m_progress_value = 0.0;
    std::shared_ptr<const Icon> m_overlay;
    std::wstring m_overlay_description;
    std::vector<ThumbnailButton> m_buttons;
    std::wstring m_tooltip;
    std::optional<RECT> m_clip;
    ThumbnailClickFn m_on_click;
};

} // namespace boson::shell
