#pragma once

#include <windows.h>

#include <boson/native/tray.hpp>

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

/// @brief Former name of TrayIcon (include/boson/native/tray.hpp), which superseded it.
using SystemTray = TrayIcon;

} // namespace boson::native
