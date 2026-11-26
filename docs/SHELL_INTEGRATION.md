# Shell Integration: Taskbar, Jump Lists, Toasts, Power, Auto-launch

Headers: `include/boson/shell/*.hpp` (native API, namespace `boson::shell`) and
`include/boson/ipc/controllers/*.hpp` (JS controllers, namespace `boson::ipc`). Everything is included by
`boson/boson.hpp`. Windows 10/11 only; uses only the Windows SDK (no Windows App SDK, no MSIX).

## 1. AppUserModelID

Taskbar grouping, jump lists and toasts all key off the AppUserModelID (AUMID). Set it first thing in
`wWinMain`, before any window is created:

```cpp
boson::shell::set_app_user_model_id(L"MyCompany.MyApp");     // SetCurrentProcessExplicitAppUserModelID
boson::shell::app_user_model_id();                              // std::optional<std::wstring>
boson::shell::set_window_app_user_model_id(hwnd, L"MyCompany.MyApp.Doc"); // per-window grouping; L"" removes
```

## 2. Taskbar (`shell/taskbar.hpp`)

`Taskbar` wraps `ITaskbarList3` for one `Window`. It hooks the window with `add_message_hook` and caches
its state. When the shell sends `TaskbarButtonCreated` (first show, or Explorer restart), it creates a new
`ITaskbarList3` and re-applies progress, overlay, thumbnail buttons, tooltip and clip. Use it on the UI thread only.

```cpp
boson::shell::Taskbar taskbar(window);
taskbar.set_progress_state(boson::shell::TaskbarProgressState::Indeterminate);
taskbar.set_progress(0.42);                 // or set_progress(done, total); Error/Paused are kept
taskbar.set_overlay_icon(std::make_shared<const boson::shell::Icon>(boson::shell::Icon::badge(L"3")), L"3 unread");
taskbar.flash({.count = 0, .until_foreground = true});   // FlashWindowEx (FLASHW_TIMERNOFG)

auto play = std::make_shared<const boson::shell::Icon>(boson::shell::Icon::glyph(0xE768));
taskbar.set_thumbnail_buttons({{.id = 1, .icon = play, .tooltip = L"Play"}});   // max 7, unique ids
taskbar.set_thumbnail_click_callback([](std::uint32_t id) { /* THBN_CLICKED */ });
taskbar.set_thumbnail_tooltip(L"Now playing");
taskbar.set_thumbnail_clip(RECT{0, 0, 400, 300});   // client px; std::nullopt = whole window
```

The shell lets a window add thumbnail buttons only once, so Boson always adds 7 slots and hides the ones it
does not use. That way `set_thumbnail_buttons` can replace the set at any time. `Icon` is a move-only HICON
owner. It can come from `from_file` (`"path,index"`), `from_image_bytes`, `stock(L"warning")`, `badge(text)` or `glyph(codepoint)`
(Segoe Fluent Icons, falling back to MDL2).

## 3. Jump lists (`shell/jump_list.hpp`)

These functions do COM and disk work, so call them off the UI thread. They initialise COM for the call when needed.

```cpp
boson::shell::JumpListSpec spec;
spec.tasks = {{.title = L"New window", .arguments = L"--new-window"}, {.separator = true},
              {.title = L"Settings", .arguments = L"--settings", .icon_path = L"C:\\...\\app.exe", .icon_index = 1}};
spec.categories = {{.name = L"Projects", .items = {{.title = L"Boson", .arguments = L"--open D:\\boson"}}}};
spec.show_recent = true;
auto r = boson::shell::set_jump_list(spec);   // r.ok, r.max_slots, r.skipped_removed, r.rejected_categories
boson::shell::add_recent_document(L"C:\\docs\\a.txt");   // SHAddToRecentDocs(SHARD_APPIDINFO)
boson::shell::clear_recent_documents();
boson::shell::clear_jump_list();
```

An empty `program` means the running executable. Items the user removed from the list are skipped (the shell
reports them in `BeginList`). A custom category is rejected with `E_ACCESSDENIED` when the user has turned off
"Show recently opened items".

## 4. Toast notifications (`shell/toast.hpp`, `shell/toast_activation.hpp`)

`ToastNotifier` calls `Windows.UI.Notifications` through the raw WinRT ABI (`roapi`, WRL). Content is built as
ToastGeneric XML (`build_toast_xml`) and supports title, body, attribution, app logo (optionally circular), hero and
inline images, up to 5 buttons (foreground/background/protocol, context-menu placement, icons), up to 5 inputs
(text = inline reply, or selection), a data-bound progress bar, scenario, duration, silent or custom/looping
sound, tag/group, suppress-popup and expiration.

```cpp
boson::shell::ToastNotifier toasts;           // uses the process AUMID at construction
toasts.set_event_callback([](const boson::shell::ToastEvent& e) { /* Activated / Dismissed / Failed; WinRT thread */ });
auto shown = toasts.show({
    .title = "Download", .body = "report.pdf",
    .buttons = {{.content = "Reply", .arguments = "reply", .activation = boson::shell::ToastActivationType::Background, .input_id = "msg"}},
    .inputs = {{.id = "msg", .placeholder = "Type a reply"}},
    .progress = boson::shell::ToastProgress{.title = "report.pdf", .value = 0.1, .status = "Downloading"},
    .tag = "dl-1"});
toasts.update_progress("dl-1", "", {.value = 0.8, .status = "Almost done"});
toasts.hide("dl-1"); toasts.remove("dl-1"); toasts.clear_all();
toasts.setting();   // Enabled / Disabled* / Unknown (Unknown is normal for an unpackaged app before its first toast)
```

### Making toasts work for an unpackaged app

Windows only shows toasts for an AUMID that the shell knows. There are two ways to register it, and both are
per-user and need no elevation:

1. **Registry**: `register_app_user_model_id({.app_user_model_id, .display_name, .icon_path, .activator_clsid})` writes
   `HKCU\Software\Classes\AppUserModelId\<id>` (`DisplayName`, `IconUri`, `CustomActivator`).
2. **Start-menu shortcut**: `create_start_menu_shortcut({.name, .folder, .app_user_model_id, .toast_activator_clsid})` writes
   `%APPDATA%\Microsoft\Windows\Start Menu\Programs\[folder\]name.lnk` with `System.AppUserModel.ID` and
   `System.AppUserModel.ToastActivatorCLSID`. `remove_start_menu_shortcut` deletes the shortcut, and its folder too when it becomes empty.
   `read_shortcut` reads both properties back.

An installer would normally do one of these. Their inverses are `unregister_app_user_model_id` and
`remove_start_menu_shortcut`.

### Activation (clicks, buttons, inline reply)

- **While the app is running**, `ToastNotifier`'s `Activated` event delivers `arguments` and `user_input` (read from
  `IToastActivatedEventArgs2::UserInput`).
- **COM activator**: `ToastActivator activator(clsid, handler)` calls `CoRegisterClassObject` with an
  `INotificationActivationCallback` class object (CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE). Create it on the UI thread,
  and the shell's calls then arrive through the message loop.
- **Launch activation**: `register_toast_com_server(clsid)` writes
  `HKCU\Software\Classes\CLSID\{clsid}\LocalServer32 = "<exe>" -ToastActivated`. When a toast is clicked while
  the app is not running, COM starts the exe with `-ToastActivated`. Check that with
  `was_launched_by_toast(boson::app::command_line_args())`, create the `ToastActivator` early, and COM then calls
  `Activate`. If you also use `single_instance`, keep the activator in the primary instance. The COM runtime starts a
  new process only when no class object is registered.

```cpp
constexpr GUID kActivator = {0x1b6f4a2e, 0x3c1d, 0x4e8f, {0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};
boson::shell::set_app_user_model_id(L"MyCompany.MyApp");
boson::shell::register_toast_com_server(kActivator);                     // installer or first run
boson::shell::create_start_menu_shortcut({.name = L"My App", .app_user_model_id = L"MyCompany.MyApp",
                                          .toast_activator_clsid = kActivator});
boson::shell::ToastActivator activator(kActivator, [&](boson::shell::ToastActivation a) {
    deliver_activation(notification_controller, a);                      // -> "notification:activated" (source "activator")
});
```

When the activator is registered and the app is running, one click can be reported twice: once by the in-process
`Activated` event (`source: "toast"`) and once by COM (`source: "activator"`). Pick one in the frontend, or skip
`deliver_activation` while the process is running.

The `Shell_NotifyIconW` balloon notifier (`native::Notifier`) is unchanged. `NotificationController::show` falls
back to it when the toast cannot be shown (`fallback: true`, the default).

## 5. Power (`shell/power.hpp`)

```cpp
boson::shell::power_status();       // on_ac, has_battery, battery_percent, charging, battery_saver, seconds remaining
boson::shell::user_idle_time();     // GetLastInputInfo
boson::shell::is_session_locked();  // WTSQuerySessionInformation(WTSSessionInfoEx)

boson::shell::PowerMonitor monitor(window);   // UI thread
monitor.on([](const boson::shell::PowerEvent& e) { /* e.kind, e.status, e.display */ });

boson::shell::PowerSaveBlocker keep_awake(boson::shell::PowerSaveBlockerType::PreventDisplaySleep, L"Presenting");
boson::shell::PowerSaveBlockers blockers;     // id-addressed, thread-safe
auto id = blockers.start(boson::shell::PowerSaveBlockerType::PreventAppSuspension, L"Sync"); blockers.stop(id);
```

`PowerMonitor` registers `RegisterSuspendResumeNotification`, `RegisterPowerSettingNotification` (AC/DC, battery
%, console display state, power-saving and energy-saver status) and `WTSRegisterSessionNotification`. Its events are
Suspend, Resume (one per suspend, even though Windows sends both RESUMEAUTOMATIC and RESUMESUSPEND),
PowerSourceChanged, BatteryPercentChanged, BatterySaverChanged, DisplayStateChanged (on/off/dimmed),
SessionLocked and SessionUnlocked. The blockers use `PowerCreateRequest`/`PowerSetRequest` (SystemRequired, plus
DisplayRequired for display sleep), and the destructor clears the request.

## 6. Auto-launch (`shell/auto_launch.hpp`)

```cpp
boson::shell::enable_auto_launch({.name = L"MyCompany.MyApp", .arguments = {L"--tray"}});
boson::shell::auto_launch_status(L"MyCompany.MyApp");   // registered, enabled, disabled_by_user, executable, arguments
boson::shell::disable_auto_launch(L"MyCompany.MyApp");  // deletes the Run value and its StartupApproved entry
```

The command goes into `HKCU\...\CurrentVersion\Run` with every argument quoted. Status reads
`Explorer\StartupApproved\Run`: an odd first byte means the user disabled the entry in Task Manager or Settings.
`enable_auto_launch` re-approves the entry only when `override_user_disabled` is true (the default). Do that only
in response to an explicit user toggle.

## 7. JS controllers

Construct these on the UI thread after `window.create()` (and after `set_app_user_model_id`), add them to the
`Router`, and call `connect_events(controller, bridge)` once the `Bridge` exists. `connect_events` and the
`native_*` accessors are friend functions, so reflection-based routing never exposes them.

```cpp
boson::ipc::TaskbarController taskbar(window, marshal);
boson::ipc::NotificationController notifications(window, marshal);
boson::ipc::PowerController power(window);
boson::ipc::AutoLaunchController auto_launch(L"MyCompany.MyApp");
boson::ipc::Router<..., boson::ipc::TaskbarController, boson::ipc::NotificationController,
                   boson::ipc::PowerController, boson::ipc::AutoLaunchController> router(..., taskbar, notifications, power, auto_launch);
boson::ipc::Bridge bridge(runner, marshal, router);
connect_events(taskbar, bridge); connect_events(notifications, bridge); connect_events(power, bridge);
```

| Controller | Methods |
|---|---|
| `Taskbar` | `set_progress({state, value?})`, `clear_progress`, `set_overlay({icon, description})`, `clear_overlay`, `flash({count, until_foreground, caption, tray, interval_ms})`, `stop_flash`, `set_thumbnail_buttons({buttons})`, `set_thumbnail_tooltip({tooltip})`, `set_thumbnail_clip({x,y,width,height})` (DIPs; 0 size clears), `set_jump_list({tasks, categories, show_recent, show_frequent})`, `clear_jump_list`, `add_recent_document({path})`, `clear_recent_documents` |
| `Notification` | `show(ToastRequest)` -> `{ok, used_fallback, tag, group, error}`, `update_progress({tag, group, progress})`, `hide({tag, group})`, `remove({tag, group})`, `clear_all`, `support` |
| `Power` | `status`, `idle_time`, `idle_state({threshold_seconds})` -> active/idle/locked, `start_blocker({type, reason})` -> `{id, started}`, `stop_blocker({id})`, `is_blocker_started({id})` |
| `AutoLaunch` | `status`, `enable({args})`, `disable`. The value name and executable are fixed in C++, so a page cannot register arbitrary programs. |

Events (`boson::ipc::ShellIntegrationEvents` in `ipc/controllers/shell_events.hpp` is the event map for tsgen):
`taskbar:thumbnail-click {id}`, `notification:activated {tag, group, arguments, user_input, source}`,
`notification:dismissed {tag, group, reason}`, `notification:failed {tag, group, error}`, and
`power:suspend|resume|source-changed|battery-changed|battery-saver-changed|display-changed|lock|unlock {kind, status}`.

`frontend/src/shell.ts` provides typed wrappers: `taskbar`, `notifications`, `power` and `autoLaunch`.

```ts
import { taskbar, notifications, power } from './shell';
await taskbar.setProgress('normal', 0.3);
await notifications.show({ title: 'Build done', body: 'All green', buttons: [{ content: 'Open', arguments: 'open' }] });
power.on('resume', (e) => console.log('resumed, AC =', e.status.on_ac));
```

## Verification

`boson_shell_integration_tests` (225/225 checks on Windows 11 Pro 10.0.26300, MSVC Debug /W4, WebView2 runtime
installed) covers:

- **Pure logic**: AUMID validation, GUID round-trip, `-ToastActivated` detection, colour parsing, XML escaping,
  path-to-URI conversion, and toast XML for every feature. The XML is also parsed by `Windows.Data.Xml.Dom`. Request
  validation for button limits and bad enums is checked too.
- **Icons**: badge, stock, glyph and file icons; move semantics.
- **Taskbar** on a real window (SW_SHOWNOACTIVATE): every progress state, overlay set/clear, thumbnail toolbar
  add/replace/update, rejection of duplicate ids and of more than 7 buttons, click routing via synthetic
  `WM_COMMAND/THBN_CLICKED`, tooltip, clip, and flash/stop. Re-initialisation is checked by sending
  `TaskbarButtonCreated` by hand.
- **Jump list**: a real `ICustomDestinationList` commit with tasks, a separator, a custom category and Recent, then
  `SHAddToRecentDocs`, `RemoveAllDestinations` and `DeleteList`.
- **Toasts**: AUMID registry registration, LocalServer32 registration, and a Start-menu shortcut (with AUMID and
  ToastActivatorCLSID read back). The COM activator is registered, then a worker thread calls
  `CoCreateInstance(CLSCTX_LOCAL_SERVER)` + `Activate` with user input, which is the shell's own call path. Real silent
  toasts are shown, the progress is updated, the toast is hidden (the `Dismissed/ApplicationHidden` event was
  received), and then removed and cleared. Controller show/remove run through the router, and the balloon fallback
  is used when no AUMID is set.
- **Power**: live status, idle time and session-lock queries; both blocker types; the id registry. The monitor's
  registrations succeed (5 power settings). Suspend/resume, display, AC/DC, saver and lock/unlock handling is
  driven with synthetic `WM_POWERBROADCAST` / `WM_WTSSESSION_CHANGE` messages.
- **Auto-launch**: enable with arguments containing spaces and quotes (round-tripped), a simulated
  Task-Manager-disabled StartupApproved entry, override on and off, and controller enable/disable.
- **End to end in WebView2**: `power:*` and `taskbar:thumbnail-click` events reach the page, and `Power.status` RPC
  works from JS.

Every name the tests use is unique per run and removed at the end: the Run/StartupApproved values, the
AppUserModelId and CLSID keys, the Start-menu folder, the Recent-items shortcut, new jump-list destination files,
and the per-AUMID `Notifications\Settings` key. A post-run scan of the registry and those folders found nothing left.

**Not verified:**
- No real sleep/resume, AC unplug, battery change or display-off was triggered; that handling was only driven by synthetic messages. This machine has no battery.
- A real Explorer restart was not performed.
- Toast clicks, button presses and inline-reply text were not tested from the Action Center. Doing that needs user interaction, so the activation path was tested only through COM.
- Launch activation of a non-running exe by COM (`-ToastActivated`) was not tested.
- The jump list was not inspected visually.
- The Windows notification platform may keep its own database entry for the test AUMID (`wpndatabase.db`). That database cannot be edited through a public API.
