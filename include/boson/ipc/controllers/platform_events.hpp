#pragma once

#include <boson/ipc/controllers/desktop_events.hpp>
#include <boson/ipc/controllers/fs_controller.hpp>
#include <boson/ipc/controllers/menu_controller.hpp>
#include <boson/ipc/controllers/shell_events.hpp>
#include <boson/ipc/controllers/titlebar_controller.hpp>
#include <boson/ipc/controllers/updater_controller.hpp>
#include <boson/ipc/controllers/webview_controller.hpp>
#include <boson/ipc/ts_generator.hpp>

#include <glaze/glaze.hpp>

namespace boson::ipc {

/// @brief Events of the titlebar, updater, scoped-fs and webview-feature controllers that have no event map
/// of their own. Names match the Bridge::emit calls in their controller headers.
struct PlatformEvents {
    TitlebarOverlayEvent titlebar_overlay;
    ThemeChangedEvent theme_changed;
    UpdateProgressEvent updater_progress;
    UpdateResultEvent updater_downloaded;
    UpdateResultEvent updater_error;
    security::FsChangeBatch fs_changed;
    FindStatus webview_find;
};

/// @brief Every built-in Boson event, for TsGenerator::generate_with_events. Combine with app events via
/// TsGenerator::EventMaps<AllBosonEvents..., MyEvents> or by listing the maps directly.
using AllBosonEvents = TsGenerator::EventMaps<DesktopEvents, MenuEvents, ShellIntegrationEvents, PlatformEvents>;

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::PlatformEvents> {
    using T = boson::ipc::PlatformEvents;
    static constexpr auto value = glz::object(
        "titlebar:overlay", &T::titlebar_overlay,
        "theme:changed", &T::theme_changed,
        "updater:progress", &T::updater_progress,
        "updater:downloaded", &T::updater_downloaded,
        "updater:error", &T::updater_error,
        "fs:changed", &T::fs_changed,
        "webview:find", &T::webview_find);
};
