#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace boson::native {

enum class NotificationLevel {
    Info,
    Warning,
    Error,
    None,
};

struct NotificationOptions {
    std::string title;
    std::string body;
    NotificationLevel level = NotificationLevel::Info;
    bool silent = false;
};

/// @brief Sends native Windows balloon/toast notifications via Shell_NotifyIconW without requiring MSIX packaging.
class Notifier {
public:
    explicit Notifier(HWND owner = nullptr) noexcept : m_owner(owner) {}
    ~Notifier();

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    Notifier(Notifier&&) = delete;
    Notifier& operator=(Notifier&&) = delete;

    void set_owner(HWND owner) noexcept { m_owner = owner; }
    bool show(const NotificationOptions& options);
    void clear() noexcept;

private:
    bool ensure_icon() noexcept;

    HWND m_owner = nullptr;
    UINT m_icon_id = 1;
    bool m_registered = false;
};

/// @brief RAII wrapper for a persistent Windows System Tray icon.
class SystemTray {
public:
    static constexpr UINT kWmTrayCallback = WM_APP + 2;

    explicit SystemTray(HWND owner = nullptr) noexcept : m_owner(owner) {}
    ~SystemTray();

    SystemTray(const SystemTray&) = delete;
    SystemTray& operator=(const SystemTray&) = delete;

    bool show(HWND owner, std::string_view tooltip);
    bool update_tooltip(std::string_view tooltip) noexcept;
    void hide() noexcept;

private:
    HWND m_owner = nullptr;
    UINT m_icon_id = 2;
    bool m_visible = false;
};

} // namespace boson::native
