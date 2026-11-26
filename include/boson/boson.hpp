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
#include <boson/ipc/sync_boot.hpp>
#include <boson/ipc/sync_bridge.hpp>
#include <boson/ipc/system_controller.hpp>
#include <boson/ipc/ts_generator.hpp>
#include <boson/ipc/controllers/auto_launch_controller.hpp>
#include <boson/ipc/controllers/notification_controller.hpp>
#include <boson/ipc/controllers/power_controller.hpp>
#include <boson/ipc/controllers/shell_common.hpp>
#include <boson/ipc/controllers/shell_events.hpp>
#include <boson/ipc/controllers/taskbar_controller.hpp>
#include <boson/ipc/controllers/webview_controller.hpp>
#include <boson/ipc/controllers/guest_controller.hpp>

#include <boson/webview/capture.hpp>
#include <boson/webview/cdp.hpp>
#include <boson/webview/cdp_server.hpp>
#include <boson/webview/cookies.hpp>
#include <boson/webview/drm.hpp>
#include <boson/webview/element.hpp>
#include <boson/webview/find.hpp>
#include <boson/webview/guest.hpp>
#include <boson/webview/print.hpp>
#include <boson/webview/profile.hpp>
#include <boson/webview/proxy.hpp>
#include <boson/webview/security_events.hpp>
#include <boson/ipc/controllers/titlebar_controller.hpp>
#include <boson/ipc/controllers/menu_controller.hpp>
#include <boson/ipc/controllers/transfer_controller.hpp>

#include <boson/native/browser_args.hpp>
#include <boson/native/dialog.hpp>
#include <boson/native/message_box.hpp>
#include <boson/native/metrics.hpp>
#include <boson/native/notification.hpp>
#include <boson/native/permissions.hpp>
#include <boson/native/session.hpp>
#include <boson/native/system_info.hpp>
#include <boson/native/tray.hpp>
#include <boson/native/web_notifications.hpp>
#include <boson/native/zoom.hpp>
#include <boson/ipc/controllers/native_controller.hpp>

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
#include <boson/ipc/controllers/platform_events.hpp>
#include <boson/ipc/controllers/screen_controller.hpp>
#include <boson/ipc/controllers/shell_controller.hpp>
#include <boson/ipc/controllers/shortcut_controller.hpp>
#include <boson/ipc/controllers/store_controller.hpp>
#include <boson/titlebar/caption_accessibility.hpp>
#include <boson/titlebar/caption_buttons.hpp>
#include <boson/titlebar/theme.hpp>
#include <boson/titlebar/titlebar.hpp>
#include <boson/titlebar/window_chrome.hpp>

#include <boson/transfer/base64.hpp>
#include <boson/transfer/binary_transport.hpp>
#include <boson/transfer/file_drop.hpp>
#include <boson/transfer/stream_server.hpp>
#include <boson/transfer/drag_out.hpp>
#include <boson/ipc/controllers/drag_controller.hpp>

#include <boson/terminal/output_channel.hpp>
#include <boson/terminal/pty.hpp>
#include <boson/terminal/terminal_host.hpp>
#include <boson/ipc/controllers/terminal_controller.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/app/window_manager.hpp>
#include <boson/ipc/controllers/window_controller.hpp>

#include <boson/process/command_line.hpp>
#include <boson/process/pipe_ipc.hpp>
#include <boson/process/policy.hpp>
#include <boson/process/process.hpp>
#include <boson/process/sidecar.hpp>
#include <boson/ipc/controllers/process_controller.hpp>

#include <boson/security/capabilities.hpp>
#include <boson/security/common.hpp>
#include <boson/security/credential_store.hpp>
#include <boson/security/fs_scope.hpp>
#include <boson/security/origin.hpp>
#include <boson/security/safe_storage.hpp>
#include <boson/security/scoped_fs.hpp>
#include <boson/security/web_security.hpp>
#include <boson/store/store.hpp>
#include <boson/shipping/authenticode.hpp>
#include <boson/shipping/crash_reporter.hpp>
#include <boson/shipping/crypto.hpp>
#include <boson/shipping/http.hpp>
#include <boson/shipping/logger.hpp>
#include <boson/shipping/updater.hpp>
#include <boson/shipping/version.hpp>
#include <boson/shipping/webview2_runtime.hpp>

#include <boson/archive/zip.hpp>
#include <boson/crypto/hash.hpp>
#include <boson/db/database.hpp>
#include <boson/db/kv_store.hpp>
#include <boson/db/table.hpp>
#include <boson/fs/install_transaction.hpp>
#include <boson/fs/known_folders.hpp>
#include <boson/net/http_cache.hpp>
#include <boson/net/http_client.hpp>
#include <boson/net/proxy.hpp>

#include <boson/ipc/controllers/log_controller.hpp>
#include <boson/ipc/controllers/updater_controller.hpp>
#include <boson/ipc/controllers/db_controller.hpp>
#include <boson/ipc/controllers/http_controller.hpp>

#include <boson/assets/asset_registry.hpp>

#include <boson/compat/electron_ipc.hpp>

// Native GPU surfaces live in the opt-in boson_gpu library (D3D11/D3D12/DirectWrite/Vulkan interop):
// #include <boson/gpu/gpu.hpp> and link boson_gpu. See docs/GPU.md.
