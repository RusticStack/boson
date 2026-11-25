#include <boson/native/notification.hpp>

#include <boson/core/result.hpp>

#include <shellapi.h>

#include <algorithm>
#include <cstring>

namespace boson::native {

namespace {

void copy_wide_truncated(wchar_t* dst, std::size_t dst_cap, std::wstring_view src) noexcept {
    if (dst_cap == 0) {
        return;
    }
    const std::size_t count = (std::min)(src.size(), dst_cap - 1);
    std::wmemcpy(dst, src.data(), count);
    dst[count] = L'\0';
}

} // namespace

Notifier::~Notifier() {
    clear();
}

bool Notifier::ensure_icon() noexcept {
    if (m_registered) {
        return true;
    }
    if (m_owner == nullptr) {
        return false;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_owner;
    nid.uID = m_icon_id;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    copy_wide_truncated(nid.szTip, std::size(nid.szTip), L"Boson");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        return false;
    }

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    m_registered = true;
    return true;
}

bool Notifier::show(const NotificationOptions& options) {
    if (!ensure_icon()) {
        return false;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_owner;
    nid.uID = m_icon_id;
    nid.uFlags = NIF_INFO;

    const std::wstring wtitle = core::utf8_to_wide(options.title);
    const std::wstring wbody = core::utf8_to_wide(options.body);
    copy_wide_truncated(nid.szInfoTitle, std::size(nid.szInfoTitle), wtitle);
    copy_wide_truncated(nid.szInfo, std::size(nid.szInfo), wbody);

    DWORD flags = NIIF_INFO;
    switch (options.level) {
    case NotificationLevel::Info:
        flags = NIIF_INFO;
        break;
    case NotificationLevel::Warning:
        flags = NIIF_WARNING;
        break;
    case NotificationLevel::Error:
        flags = NIIF_ERROR;
        break;
    case NotificationLevel::None:
        flags = NIIF_NONE;
        break;
    }
    if (options.silent) {
        flags |= NIIF_NOSOUND;
    }
    nid.dwInfoFlags = flags;

    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

void Notifier::clear() noexcept {
    if (!m_registered || m_owner == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_owner;
    nid.uID = m_icon_id;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_registered = false;
}

SystemTray::~SystemTray() {
    hide();
}

bool SystemTray::show(HWND owner, std::string_view tooltip) {
    m_owner = owner;
    if (m_owner == nullptr) {
        return false;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_owner;
    nid.uID = m_icon_id;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = kWmTrayCallback;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    copy_wide_truncated(nid.szTip, std::size(nid.szTip), core::utf8_to_wide(tooltip));

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        return false;
    }
    m_visible = true;
    return true;
}

bool SystemTray::update_tooltip(std::string_view tooltip) noexcept {
    if (!m_visible || m_owner == nullptr) {
        return false;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_owner;
    nid.uID = m_icon_id;
    nid.uFlags = NIF_TIP;
    copy_wide_truncated(nid.szTip, std::size(nid.szTip), core::utf8_to_wide(tooltip));
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

void SystemTray::hide() noexcept {
    if (!m_visible || m_owner == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = m_owner;
    nid.uID = m_icon_id;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_visible = false;
}

} // namespace boson::native
