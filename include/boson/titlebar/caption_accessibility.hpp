#pragma once

#include <windows.h>
#include <wrl.h>

#include <boson/titlebar/window_chrome.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct IRawElementProviderSimple;

namespace boson::titlebar {

/// @brief One caption button as UI Automation sees it.
struct AccessibleButton {
    CaptionButton kind = CaptionButton::Close;
    RECT screen_rect{};
    std::wstring name;
    bool enabled = true;
};

/// @brief Callbacks an accessible caption host supplies. All are called on the host HWND's thread.
struct CaptionAccessibilitySource {
    /// Buttons in visual order (left to right). Called for every query, so names follow state changes.
    std::function<std::vector<AccessibleButton>()> buttons;
    /// Must not block: post the action (UIA waits for Invoke to return).
    std::function<void(CaptionButton)> invoke;
    /// Name of the container element (UIA TitleBar); usually the window title.
    std::function<std::wstring()> container_name;
};

/// @brief UI Automation provider tree for a window that draws caption buttons itself: a TitleBar fragment
/// root hosted by the HWND, with one Button child (Invoke pattern, localized name, AutomationId
/// "Minimize" / "Maximize" / "Close") per button. Answer WM_GETOBJECT with handle_get_object(). UI thread.
class CaptionAccessibility {
public:
    CaptionAccessibility(HWND host, CaptionAccessibilitySource source);
    ~CaptionAccessibility();

    CaptionAccessibility(const CaptionAccessibility&) = delete;
    CaptionAccessibility& operator=(const CaptionAccessibility&) = delete;
    CaptionAccessibility(CaptionAccessibility&&) = delete;
    CaptionAccessibility& operator=(CaptionAccessibility&&) = delete;

    /// @brief Returns the LRESULT for WM_GETOBJECT, or std::nullopt for object ids UIA does not own.
    std::optional<LRESULT> handle_get_object(WPARAM w_param, LPARAM l_param);
    /// @brief Raises a UIA Name property-changed event for kind (e.g. Maximize -> Restore) when clients listen.
    void notify_name_changed(CaptionButton kind, const std::wstring& old_name, const std::wstring& new_name);
    /// @brief Disconnects every provider (call before the host HWND goes away; the destructor does it too).
    void disconnect() noexcept;

    struct State;

private:
    std::shared_ptr<State> m_state;
};

/// @brief Accessibility for core::Window's HTML-button maximize overlay (BosonCaptionOverlay). Called from
/// its window procedure; parent is the top-level window.
std::optional<LRESULT> html_caption_overlay_get_object(HWND overlay, HWND parent, WPARAM w_param, LPARAM l_param);
void html_caption_overlay_destroyed(HWND overlay) noexcept;

} // namespace boson::titlebar
