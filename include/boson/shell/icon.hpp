#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace boson::shell {

/// @brief Owning (or borrowing) HICON wrapper. Move-only; DestroyIcon on destruction when owned.
class Icon {
public:
    Icon() noexcept = default;
    explicit Icon(HICON icon, bool owned = true) noexcept : m_icon(icon), m_owned(owned) {}
    ~Icon() { reset(); }

    Icon(const Icon&) = delete;
    Icon& operator=(const Icon&) = delete;
    Icon(Icon&& other) noexcept : m_icon(other.m_icon), m_owned(other.m_owned) {
        other.m_icon = nullptr;
        other.m_owned = false;
    }
    Icon& operator=(Icon&& other) noexcept {
        if (this != &other) {
            reset();
            m_icon = other.m_icon;
            m_owned = other.m_owned;
            other.m_icon = nullptr;
            other.m_owned = false;
        }
        return *this;
    }

    /// @brief Icon at index of an .ico/.exe/.dll ("path" or "path,index" accepted), scaled to size_px
    /// (0 = SM_CXSMICON).
    [[nodiscard]] static Icon from_file(std::wstring_view path, int size_px = 0);
    /// @brief Icon from encoded PNG (or ICO image resource) bytes in memory.
    [[nodiscard]] static Icon from_image_bytes(std::span<const std::byte> bytes, int size_px = 0);
    /// @brief Shell stock icon (SHGetStockIconInfo).
    [[nodiscard]] static Icon stock(SHSTOCKICONID id, bool small_icon = true);
    /// @brief Stock icon by name: info, warning, error, shield, help, delete, find, settings, folder,
    /// document, lock, key, world, users, link, printer, rename, recycler, share, software. Empty icon if unknown.
    [[nodiscard]] static Icon stock(std::wstring_view name, bool small_icon = true);
    /// @brief Round badge with up to ~3 characters of text (e.g. an unread count for an overlay icon).
    [[nodiscard]] static Icon badge(std::wstring_view text, COLORREF background = RGB(0xC4, 0x2B, 0x1C),
                                    COLORREF foreground = RGB(0xFF, 0xFF, 0xFF), int size_px = 0);
    /// @brief One glyph of the system icon font (Segoe Fluent Icons, falling back to Segoe MDL2 Assets),
    /// e.g. 0xE768 play, 0xE769 pause, 0xE892 previous, 0xE893 next. Transparent background.
    [[nodiscard]] static Icon glyph(wchar_t codepoint, COLORREF color = RGB(0xFF, 0xFF, 0xFF), int size_px = 0);

    [[nodiscard]] HICON get() const noexcept { return m_icon; }
    [[nodiscard]] bool valid() const noexcept { return m_icon != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /// @brief Gives up ownership.
    [[nodiscard]] HICON release() noexcept {
        HICON icon = m_icon;
        m_icon = nullptr;
        m_owned = false;
        return icon;
    }
    void reset() noexcept {
        if (m_icon != nullptr && m_owned) {
            ::DestroyIcon(m_icon);
        }
        m_icon = nullptr;
        m_owned = false;
    }

private:
    HICON m_icon = nullptr;
    bool m_owned = false;
};

/// @brief Default small icon size (SM_CXSMICON for the system DPI).
[[nodiscard]] int default_small_icon_size() noexcept;

} // namespace boson::shell
