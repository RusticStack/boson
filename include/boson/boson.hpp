#pragma once

#include <boson/core/composition.hpp>
#include <boson/core/environment.hpp>
#include <boson/core/inplace_vector.hpp>
#include <boson/core/protocol.hpp>
#include <boson/core/result.hpp>
#include <boson/core/ui_timer.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/webview_events.hpp>
#include <boson/core/webview_host.hpp>
#include <boson/core/window.hpp>

#include <boson/exec/callback_sender.hpp>
#include <boson/exec/scheduler.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/exec/ui_marshal.hpp>

#include <boson/ipc/bridge.hpp>
#include <boson/ipc/controllers/credential_controller.hpp>
#include <boson/ipc/controllers/fs_controller.hpp>
#include <boson/ipc/controllers/safe_storage_controller.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/protocol_types.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/ipc/router.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>
#include <boson/ipc/controllers/auto_launch_controller.hpp>
#include <boson/ipc/controllers/notification_controller.hpp>
#include <boson/ipc/controllers/power_controller.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/controllers/shell_events.hpp>
#include <boson/ipc/controllers/taskbar_controller.hpp>
#include <boson/ipc/controllers/webview_controller.hpp>

#include <boson/webview/capture.hpp>
#include <boson/webview/cookies.hpp>
#include <boson/webview/find.hpp>
#include <boson/webview/print.hpp>
#include <boson/webview/profile.hpp>
#include <boson/webview/proxy.hpp>
#include <boson/webview/security_events.hpp>
#include <boson/ipc/controllers/titlebar_controller.hpp>
#include <boson/ipc/controllers/menu_controller.hpp>

#include <boson/native/dialog.hpp>
#include <boson/native/notification.hpp>
#include <boson/native/tray.hpp>

#include <boson/menu/accelerator.hpp>
#include <boson/menu/menu.hpp>
#include <boson/menu/menu_bar.hpp>

#include <boson/shell/app_id.hpp>
#include <boson/shell/auto_launch.hpp>
#include <boson/shell/icon.hpp>
#include <boson/shell/jump_list.hpp>
#include <boson/shell/power.hpp>
#include <boson/shell/taskbar.hpp>
#include <boson/shell/toast.hpp>
#include <boson/shell/toast_activation.hpp>
#include <boson/desktop/clipboard.hpp>
#include <boson/desktop/deep_link.hpp>
#include <boson/desktop/screen.hpp>
#include <boson/desktop/shell.hpp>
#include <boson/desktop/shortcuts.hpp>
#include <boson/desktop/window_state.hpp>
#include <boson/ipc/controllers/clipboard_controller.hpp>
#include <boson/ipc/controllers/desktop_events.hpp>
#include <boson/ipc/controllers/screen_controller.hpp>
#include <boson/ipc/controllers/shell_controller.hpp>
#include <boson/ipc/controllers/shortcut_controller.hpp>
#include <boson/titlebar/caption_accessibility.hpp>
#include <boson/titlebar/caption_buttons.hpp>
#include <boson/titlebar/theme.hpp>
#include <boson/titlebar/titlebar.hpp>
#include <boson/titlebar/window_chrome.hpp>

#include <boson/app/lifecycle.hpp>

#include <boson/security/capabilities.hpp>
#include <boson/security/common.hpp>
#include <boson/security/credential_store.hpp>
#include <boson/security/fs_scope.hpp>
#include <boson/security/origin.hpp>
#include <boson/security/safe_storage.hpp>
#include <boson/security/scoped_fs.hpp>
#include <boson/security/web_security.hpp>
#include <boson/shipping/authenticode.hpp>
#include <boson/shipping/crash_reporter.hpp>
#include <boson/shipping/crypto.hpp>
#include <boson/shipping/http.hpp>
#include <boson/shipping/logger.hpp>
#include <boson/shipping/updater.hpp>
#include <boson/shipping/version.hpp>
#include <boson/shipping/webview2_runtime.hpp>

#include <boson/ipc/controllers/log_controller.hpp>
#include <boson/ipc/controllers/updater_controller.hpp>

#include <boson/assets/asset_registry.hpp>

#include <boson/compat/electron_ipc.hpp>
