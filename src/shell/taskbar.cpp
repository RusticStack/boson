#include <boson/shell/taskbar.hpp>

#include <boson/core/result.hpp>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace boson::shell {

namespace {

TBPFLAG to_tbpf(TaskbarProgressState state) noexcept {
    switch (state) {
    case TaskbarProgressState::None: return TBPF_NOPROGRESS;
    case TaskbarProgressState::Indeterminate: return TBPF_INDETERMINATE;
    case TaskbarProgressState::Normal: return TBPF_NORMAL;
    case TaskbarProgressState::Error: return TBPF_ERROR;
    case TaskbarProgressState::Paused: return TBPF_PAUSED;
    }
    return TBPF_NOPROGRESS;
}

constexpr ULONGLONG kProgressScale = 10000;

} // namespace

bool flash_window(HWND hwnd, const FlashOptions& options) noexcept {
    if (hwnd == nullptr) {
        return false;
    }
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = hwnd;
    DWORD flags = 0;
    if (options.caption) flags |= FLASHW_CAPTION;
    if (options.tray) flags |= FLASHW_TRAY;
    if (flags == 0) flags = FLASHW_ALL;
    if (options.until_foreground) {
        flags |= FLASHW_TIMERNOFG;
    } else if (options.count == 0) {
        flags |= FLASHW_TIMER;
    }
    info.dwFlags = flags;
    info.uCount = options.count;
    info.dwTimeout = static_cast<DWORD>(std::max<long long>(0, options.interval.count()));
    ::FlashWindowEx(&info);
    return true;
}

bool stop_flash_window(HWND hwnd) noexcept {
    if (hwnd == nullptr) {
        return false;
    }
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = hwnd;
    info.dwFlags = FLASHW_STOP;
    ::FlashWindowEx(&info);
    return true;
}

UINT Taskbar::taskbar_button_created_message() noexcept {
    static const UINT message = ::RegisterWindowMessageW(L"TaskbarButtonCreated");
    return message;
}

Taskbar::Taskbar(core::Window& window) : m_window(window) {
    const HWND hwnd = m_window.hwnd();
    if (hwnd != nullptr) {
        // Elevated processes must let Explorer's (lower integrity) messages through.
        ::ChangeWindowMessageFilterEx(hwnd, taskbar_button_created_message(), MSGFLT_ALLOW, nullptr);
        ::ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);
    }
    m_hook = m_window.add_message_hook([this](UINT msg, WPARAM w_param, LPARAM l_param) {
        return on_message(msg, w_param, l_param);
    });
    // The button usually exists already (the window was shown before this object was created).
    ensure_taskbar();
}

Taskbar::~Taskbar() {
    m_window.remove_message_hook(m_hook);
    const HWND hwnd = m_window.hwnd();
    if (m_taskbar != nullptr && hwnd != nullptr && ::IsWindow(hwnd)) {
        m_taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
        m_taskbar->SetOverlayIcon(hwnd, nullptr, nullptr);
        if (m_buttons_added) {
            m_buttons.clear();
            auto slots = build_slots();
            m_taskbar->ThumbBarUpdateButtons(hwnd, static_cast<UINT>(slots.size()), slots.data());
        }
        m_taskbar->SetThumbnailClip(hwnd, nullptr);
    }
}

bool Taskbar::ensure_taskbar() {
    if (m_taskbar != nullptr) {
        return true;
    }
    Microsoft::WRL::ComPtr<ITaskbarList3> list;
    if (!BOSON_HR_CHECK(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) {
        return false;
    }
    if (!BOSON_HR_CHECK(list->HrInit())) {
        return false;
    }
    m_taskbar = std::move(list);
    return true;
}

void Taskbar::reinitialize() {
    ++m_created_count;
    // After an Explorer restart the old ITaskbarList3 talks to a dead process and the buttons are gone.
    m_taskbar.Reset();
    m_buttons_added = false;
    if (!ensure_taskbar()) {
        return;
    }
    apply_progress();
    apply_overlay();
    apply_buttons();
    apply_tooltip();
    apply_clip();
}

std::optional<LRESULT> Taskbar::on_message(UINT msg, WPARAM w_param, LPARAM /*l_param*/) {
    if (msg == taskbar_button_created_message() && msg != 0) {
        reinitialize();
        return std::nullopt; // other listeners may care too
    }
    if (msg == WM_COMMAND && HIWORD(w_param) == THBN_CLICKED) {
        const UINT command = LOWORD(w_param);
        if (command >= kSlotBaseId && command < kSlotBaseId + kMaxThumbnailButtons) {
            const std::size_t slot = command - kSlotBaseId;
            if (slot < m_buttons.size() && m_on_click) {
                const std::uint32_t id = m_buttons[slot].id;
                // Copy: the callback may replace the buttons.
                const ThumbnailClickFn callback = m_on_click;
                callback(id);
            }
            return 0;
        }
    }
    return std::nullopt;
}

bool Taskbar::apply_progress() {
    if (!ensure_taskbar()) {
        return false;
    }
    const HWND hwnd = m_window.hwnd();
    bool ok = BOSON_HR_CHECK(m_taskbar->SetProgressState(hwnd, to_tbpf(m_progress_state)));
    if (m_progress_state == TaskbarProgressState::Normal || m_progress_state == TaskbarProgressState::Error ||
        m_progress_state == TaskbarProgressState::Paused) {
        const auto completed = static_cast<ULONGLONG>(std::llround(m_progress_value * static_cast<double>(kProgressScale)));
        ok = BOSON_HR_CHECK(m_taskbar->SetProgressValue(hwnd, completed, kProgressScale)) && ok;
        // SetProgressValue may flip Error/Paused back to Normal on some builds; re-assert the state.
        ok = BOSON_HR_CHECK(m_taskbar->SetProgressState(hwnd, to_tbpf(m_progress_state))) && ok;
    }
    return ok;
}

bool Taskbar::set_progress_state(TaskbarProgressState state) {
    m_progress_state = state;
    if (state == TaskbarProgressState::None) {
        m_progress_value = 0.0;
    }
    return apply_progress();
}

bool Taskbar::set_progress(double fraction) {
    if (!std::isfinite(fraction)) {
        return false;
    }
    m_progress_value = std::clamp(fraction, 0.0, 1.0);
    if (m_progress_state == TaskbarProgressState::None || m_progress_state == TaskbarProgressState::Indeterminate) {
        m_progress_state = TaskbarProgressState::Normal;
    }
    return apply_progress();
}

bool Taskbar::set_progress(std::uint64_t completed, std::uint64_t total) {
    if (total == 0) {
        return set_progress(0.0);
    }
    return set_progress(static_cast<double>((std::min)(completed, total)) / static_cast<double>(total));
}

bool Taskbar::apply_overlay() {
    if (!ensure_taskbar()) {
        return false;
    }
    const HICON icon = m_overlay != nullptr ? m_overlay->get() : nullptr;
    return BOSON_HR_CHECK(m_taskbar->SetOverlayIcon(
        m_window.hwnd(), icon, icon != nullptr && !m_overlay_description.empty() ? m_overlay_description.c_str() : nullptr));
}

bool Taskbar::set_overlay_icon(std::shared_ptr<const Icon> icon, std::wstring_view description) {
    if (icon == nullptr || !icon->valid()) {
        return clear_overlay_icon();
    }
    m_overlay = std::move(icon);
    m_overlay_description.assign(description);
    return apply_overlay();
}

bool Taskbar::clear_overlay_icon() {
    m_overlay.reset();
    m_overlay_description.clear();
    return apply_overlay();
}

bool Taskbar::flash(const FlashOptions& options) const noexcept {
    return flash_window(m_window.hwnd(), options);
}

bool Taskbar::stop_flash() const noexcept {
    return stop_flash_window(m_window.hwnd());
}

std::array<THUMBBUTTON, Taskbar::kMaxThumbnailButtons> Taskbar::build_slots() const {
    std::array<THUMBBUTTON, kMaxThumbnailButtons> slots{};
    for (std::size_t i = 0; i < slots.size(); ++i) {
        THUMBBUTTON& slot = slots[i];
        slot.dwMask = static_cast<THUMBBUTTONMASK>(THB_FLAGS | THB_ICON | THB_TOOLTIP);
        slot.iId = slot_command_id(i);
        if (i >= m_buttons.size()) {
            slot.dwFlags = static_cast<THUMBBUTTONFLAGS>(THBF_HIDDEN);
            continue;
        }
        const ThumbnailButton& button = m_buttons[i];
        int flags = button.enabled ? THBF_ENABLED : THBF_DISABLED;
        if (button.hidden) flags |= THBF_HIDDEN;
        if (button.dismiss_on_click) flags |= THBF_DISMISSONCLICK;
        if (button.no_background) flags |= THBF_NOBACKGROUND;
        if (button.non_interactive) flags |= THBF_NONINTERACTIVE;
        slot.dwFlags = static_cast<THUMBBUTTONFLAGS>(flags);
        slot.hIcon = button.icon != nullptr ? button.icon->get() : nullptr;
        const std::size_t count = (std::min)(button.tooltip.size(), std::size(slot.szTip) - 1);
        std::wmemcpy(slot.szTip, button.tooltip.data(), count);
        slot.szTip[count] = L'\0';
    }
    return slots;
}

bool Taskbar::apply_buttons() {
    if (!ensure_taskbar()) {
        return false;
    }
    if (!m_buttons_added && m_buttons.empty()) {
        return true; // nothing to show; do not create the toolbar yet
    }
    const HWND hwnd = m_window.hwnd();
    auto slots = build_slots();
    if (!m_buttons_added) {
        if (!BOSON_HR_CHECK(m_taskbar->ThumbBarAddButtons(hwnd, static_cast<UINT>(slots.size()), slots.data()))) {
            return false;
        }
        m_buttons_added = true;
        return true;
    }
    return BOSON_HR_CHECK(m_taskbar->ThumbBarUpdateButtons(hwnd, static_cast<UINT>(slots.size()), slots.data()));
}

bool Taskbar::set_thumbnail_buttons(std::vector<ThumbnailButton> buttons) {
    if (buttons.size() > kMaxThumbnailButtons) {
        return false;
    }
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        for (std::size_t j = i + 1; j < buttons.size(); ++j) {
            if (buttons[i].id == buttons[j].id) {
                return false;
            }
        }
    }
    m_buttons = std::move(buttons);
    return apply_buttons();
}

bool Taskbar::update_thumbnail_button(const ThumbnailButton& button) {
    for (auto& existing : m_buttons) {
        if (existing.id == button.id) {
            existing = button;
            return apply_buttons();
        }
    }
    return false;
}

bool Taskbar::apply_tooltip() {
    if (!ensure_taskbar()) {
        return false;
    }
    return BOSON_HR_CHECK(m_taskbar->SetThumbnailTooltip(m_window.hwnd(), m_tooltip.empty() ? nullptr : m_tooltip.c_str()));
}

bool Taskbar::set_thumbnail_tooltip(std::wstring_view tooltip) {
    m_tooltip.assign(tooltip);
    return apply_tooltip();
}

bool Taskbar::apply_clip() {
    if (!ensure_taskbar()) {
        return false;
    }
    if (!m_clip.has_value()) {
        return BOSON_HR_CHECK(m_taskbar->SetThumbnailClip(m_window.hwnd(), nullptr));
    }
    RECT clip = *m_clip;
    return BOSON_HR_CHECK(m_taskbar->SetThumbnailClip(m_window.hwnd(), &clip));
}

bool Taskbar::set_thumbnail_clip(std::optional<RECT> client_px) {
    if (client_px.has_value() && (client_px->right <= client_px->left || client_px->bottom <= client_px->top)) {
        return false;
    }
    m_clip = client_px;
    return apply_clip();
}

} // namespace boson::shell
