#pragma once

#include <boson/core/result.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/export.hpp>
#include <boson/shell/jump_list.hpp>
#include <boson/shell/taskbar.hpp>

#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace boson::ipc {

struct TaskbarProgressRequest {
    std::string state = "normal"; ///< none | indeterminate | normal | error | paused
    std::optional<double> value;  ///< 0..1
};

struct TaskbarOverlayRequest {
    IconSpec icon;
    std::string description;
};

struct TaskbarFlashRequest {
    std::uint32_t count = 3;
    bool until_foreground = false;
    bool caption = true;
    bool tray = true;
    std::uint32_t interval_ms = 0;
};

struct ThumbnailButtonSpec {
    std::uint32_t id = 0;
    IconSpec icon;
    std::string tooltip;
    bool enabled = true;
    bool hidden = false;
    bool dismiss_on_click = false;
    bool no_background = false;
};

struct ThumbnailButtonsRequest {
    std::vector<ThumbnailButtonSpec> buttons;
};

struct ThumbnailTooltipRequest {
    std::string tooltip;
};

/// @brief Client-area rect in DIPs; width or height 0 clears the clip.
struct ThumbnailClipRequest {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct JumpListItemSpec {
    std::string title;
    std::string description;
    std::string arguments; ///< passed to this executable
    std::string icon_path; ///< empty = this executable
    int icon_index = 0;
    bool separator = false;
};

struct JumpListCategorySpec {
    std::string name;
    std::vector<JumpListItemSpec> items;
};

struct JumpListRequest {
    std::vector<JumpListItemSpec> tasks;
    std::vector<JumpListCategorySpec> categories;
    bool show_recent = false;
    bool show_frequent = false;
};

struct JumpListResponse {
    bool ok = false;
    std::uint32_t max_slots = 0;
    std::uint32_t skipped_removed = 0;
    std::vector<std::string> rejected_categories;
    std::string error;
};

struct RecentDocumentRequest {
    std::string path;
};

struct ThumbnailClickEvent {
    std::uint32_t id = 0;
};

[[nodiscard]] inline shell::TaskbarProgressState parse_taskbar_progress_state(std::string_view state) {
    if (state == "none") return shell::TaskbarProgressState::None;
    if (state == "indeterminate") return shell::TaskbarProgressState::Indeterminate;
    if (state == "normal") return shell::TaskbarProgressState::Normal;
    if (state == "error") return shell::TaskbarProgressState::Error;
    if (state == "paused") return shell::TaskbarProgressState::Paused;
    throw std::invalid_argument("Unsupported progress state: " + std::string(state));
}

/// @brief Taskbar button + jump list for one Window. Construct on the UI thread after window.create().
/// Thumbnail clicks are emitted as "taskbar:thumbnail-click" once connect_events(controller, bridge) was called.
class TaskbarController {
public:
    BOSON_CONTROLLER("Taskbar");

    TaskbarController(core::Window& window, exec::UiMarshal& marshal)
        : m_window(window), m_marshal(marshal), m_taskbar(std::make_unique<shell::Taskbar>(window)) {
        m_taskbar->set_thumbnail_click_callback([this](std::uint32_t id) {
            m_events.emit("taskbar:thumbnail-click", ThumbnailClickEvent{.id = id});
        });
    }

    // Non-RPC helpers are friends, so reflection-based routing never exposes them.
    friend void connect_events(TaskbarController& controller, Bridge& bridge) noexcept { controller.m_events.set(&bridge); }
    /// @brief The underlying Taskbar (UI thread only).
    friend shell::Taskbar& native_taskbar(TaskbarController& controller) noexcept { return *controller.m_taskbar; }

    [[nodiscard]] ShellResult set_progress(TaskbarProgressRequest req) const {
        const auto state = parse_taskbar_progress_state(req.state);
        return m_marshal.invoke_sync([this, state, value = req.value]() {
            bool ok = m_taskbar->set_progress_state(state);
            if (ok && value.has_value() && state != shell::TaskbarProgressState::None &&
                state != shell::TaskbarProgressState::Indeterminate) {
                ok = m_taskbar->set_progress(*value);
                ok = m_taskbar->set_progress_state(state) && ok;
            }
            return ShellResult{.ok = ok, .error = ok ? "" : "ITaskbarList3 unavailable"};
        });
    }

    [[nodiscard]] ShellResult clear_progress() const {
        return m_marshal.invoke_sync([this]() { return ShellResult{.ok = m_taskbar->clear_progress()}; });
    }

    [[nodiscard]] ShellResult set_overlay(TaskbarOverlayRequest req) const {
        auto icon = shell_detail::make_icon(req.icon);
        return m_marshal.invoke_sync([this, icon = std::move(icon), desc = core::utf8_to_wide(req.description)]() mutable {
            const bool ok = icon != nullptr ? m_taskbar->set_overlay_icon(std::move(icon), desc) : m_taskbar->clear_overlay_icon();
            return ShellResult{.ok = ok};
        });
    }

    [[nodiscard]] ShellResult clear_overlay() const {
        return m_marshal.invoke_sync([this]() { return ShellResult{.ok = m_taskbar->clear_overlay_icon()}; });
    }

    /// FlashWindowEx is thread-safe; no UI round trip needed.
    [[nodiscard]] ShellResult flash(TaskbarFlashRequest req) const {
        const bool ok = shell::flash_window(m_window.hwnd(), shell::FlashOptions{
            .count = req.count,
            .until_foreground = req.until_foreground,
            .caption = req.caption,
            .tray = req.tray,
            .interval = std::chrono::milliseconds{req.interval_ms},
        });
        return ShellResult{.ok = ok};
    }

    [[nodiscard]] ShellResult stop_flash() const {
        return ShellResult{.ok = shell::stop_flash_window(m_window.hwnd())};
    }

    [[nodiscard]] ShellResult set_thumbnail_buttons(ThumbnailButtonsRequest req) const {
        if (req.buttons.size() > shell::Taskbar::kMaxThumbnailButtons) {
            throw std::invalid_argument("At most 7 thumbnail buttons are supported");
        }
        std::vector<shell::ThumbnailButton> buttons;
        buttons.reserve(req.buttons.size());
        for (const auto& spec : req.buttons) {
            buttons.push_back(shell::ThumbnailButton{
                .id = spec.id,
                .icon = shell_detail::make_icon(spec.icon),
                .tooltip = core::utf8_to_wide(spec.tooltip),
                .enabled = spec.enabled,
                .hidden = spec.hidden,
                .dismiss_on_click = spec.dismiss_on_click,
                .no_background = spec.no_background,
                .non_interactive = false,
            });
        }
        return m_marshal.invoke_sync([this, buttons = std::move(buttons)]() mutable {
            const bool ok = m_taskbar->set_thumbnail_buttons(std::move(buttons));
            return ShellResult{.ok = ok, .error = ok ? "" : "Thumbnail toolbar rejected (duplicate ids or no taskbar)"};
        });
    }

    [[nodiscard]] ShellResult set_thumbnail_tooltip(ThumbnailTooltipRequest req) const {
        return m_marshal.invoke_sync([this, tip = core::utf8_to_wide(req.tooltip)]() {
            return ShellResult{.ok = m_taskbar->set_thumbnail_tooltip(tip)};
        });
    }

    [[nodiscard]] ShellResult set_thumbnail_clip(ThumbnailClipRequest req) const {
        return m_marshal.invoke_sync([this, req]() {
            if (req.width <= 0.0 || req.height <= 0.0) {
                return ShellResult{.ok = m_taskbar->set_thumbnail_clip(std::nullopt)};
            }
            const double scale = static_cast<double>(m_window.dpi()) / USER_DEFAULT_SCREEN_DPI;
            const auto px = [scale](double dip) { return static_cast<LONG>(dip * scale + 0.5); };
            return ShellResult{.ok = m_taskbar->set_thumbnail_clip(RECT{
                px(req.x), px(req.y), px(req.x + req.width), px(req.y + req.height)})};
        });
    }

    /// Runs on the calling pool thread (COM/disk work, no UI affinity).
    [[nodiscard]] JumpListResponse set_jump_list(JumpListRequest req) const {
        shell::JumpListSpec spec;
        spec.show_recent = req.show_recent;
        spec.show_frequent = req.show_frequent;
        const auto convert = [](const JumpListItemSpec& item) {
            return shell::JumpListItem{
                .title = core::utf8_to_wide(item.title),
                .description = core::utf8_to_wide(item.description),
                .program = {},
                .arguments = core::utf8_to_wide(item.arguments),
                .working_directory = {},
                .icon_path = core::utf8_to_wide(item.icon_path),
                .icon_index = item.icon_index,
                .separator = item.separator,
            };
        };
        for (const auto& task : req.tasks) {
            spec.tasks.push_back(convert(task));
        }
        for (const auto& category : req.categories) {
            shell::JumpListCategory out{.name = core::utf8_to_wide(category.name), .items = {}};
            for (const auto& item : category.items) {
                out.items.push_back(convert(item));
            }
            spec.categories.push_back(std::move(out));
        }
        const shell::JumpListResult result = shell::set_jump_list(spec);
        JumpListResponse response{
            .ok = result.ok,
            .max_slots = result.max_slots,
            .skipped_removed = result.skipped_removed,
            .rejected_categories = {},
            .error = {},
        };
        for (const auto& name : result.rejected_categories) {
            response.rejected_categories.push_back(core::wide_to_utf8(name));
        }
        if (!result.ok) {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX", static_cast<unsigned long>(result.hr));
            response.error = buffer;
        }
        return response;
    }

    [[nodiscard]] ShellResult clear_jump_list() const { return ShellResult{.ok = shell::clear_jump_list()}; }

    [[nodiscard]] ShellResult add_recent_document(RecentDocumentRequest req) const {
        if (req.path.empty()) {
            throw std::invalid_argument("path must not be empty");
        }
        return ShellResult{.ok = shell::add_recent_document(core::utf8_to_wide(req.path))};
    }

    [[nodiscard]] ShellResult clear_recent_documents() const { return ShellResult{.ok = shell::clear_recent_documents()}; }

private:
    core::Window& m_window;
    exec::UiMarshal& m_marshal;
    std::unique_ptr<shell::Taskbar> m_taskbar;
    shell_detail::EventSink m_events;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::TaskbarController> {
    using T = boson::ipc::TaskbarController;
    static constexpr auto value = glz::object(
        "set_progress", &T::set_progress,
        "clear_progress", &T::clear_progress,
        "set_overlay", &T::set_overlay,
        "clear_overlay", &T::clear_overlay,
        "flash", &T::flash,
        "stop_flash", &T::stop_flash,
        "set_thumbnail_buttons", &T::set_thumbnail_buttons,
        "set_thumbnail_tooltip", &T::set_thumbnail_tooltip,
        "set_thumbnail_clip", &T::set_thumbnail_clip,
        "set_jump_list", &T::set_jump_list,
        "clear_jump_list", &T::clear_jump_list,
        "add_recent_document", &T::add_recent_document,
        "clear_recent_documents", &T::clear_recent_documents);
};
