#pragma once

#include <windows.h>
#include <objbase.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace boson::shell {

/// @brief Argument the COM runtime passes when it launches the app to deliver a toast activation
/// (the LocalServer32 command written by register_toast_com_server).
inline constexpr std::wstring_view kToastActivatedSwitch = L"-ToastActivated";

[[nodiscard]] std::optional<GUID> parse_guid(std::wstring_view text) noexcept;
/// @brief "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
[[nodiscard]] std::wstring guid_to_string(const GUID& guid);

/// @brief true when args (argv[1..]) contain kToastActivatedSwitch: the process was started by COM to
/// handle a toast click. Create the ToastActivator early; Activate() is called once it is registered.
[[nodiscard]] bool was_launched_by_toast(std::span<const std::wstring> args) noexcept;

struct ToastActivation {
    std::wstring app_user_model_id;
    std::string arguments;                        ///< the launch/button arguments of the toast
    std::map<std::string, std::string> user_input; ///< input id -> value
};

/// @brief Registers this process's INotificationActivationCallback class object (CoRegisterClassObject,
/// CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE) under clsid. The shell calls it for every toast click of an
/// AppUserModelID whose shortcut (ToastActivatorCLSID) or registry entry (CustomActivator) names clsid.
///
/// Create it on the UI (STA) thread: calls then arrive on that thread through the message loop. The
/// handler must return quickly. Revoked on destruction.
class ToastActivator {
public:
    using Handler = std::function<void(ToastActivation activation)>;

    ToastActivator(const GUID& clsid, Handler handler);
    ~ToastActivator();

    ToastActivator(const ToastActivator&) = delete;
    ToastActivator& operator=(const ToastActivator&) = delete;
    ToastActivator(ToastActivator&&) = delete;
    ToastActivator& operator=(ToastActivator&&) = delete;

    [[nodiscard]] bool is_registered() const noexcept { return m_cookie != 0; }
    [[nodiscard]] HRESULT registration_result() const noexcept { return m_hr; }
    [[nodiscard]] const GUID& clsid() const noexcept { return m_clsid; }
    void set_handler(Handler handler);

    struct State;

private:
    GUID m_clsid{};
    DWORD m_cookie = 0;
    HRESULT m_hr = E_FAIL;
    std::shared_ptr<State> m_state;
};

/// @brief HKCU\Software\Classes\CLSID\{clsid}\LocalServer32 = "<executable>" <arguments>, so the shell can
/// start the app for a toast clicked while it is not running. executable empty = this executable.
bool register_toast_com_server(const GUID& clsid, std::wstring_view executable = {},
                               std::wstring_view arguments = kToastActivatedSwitch);
bool unregister_toast_com_server(const GUID& clsid);
/// @brief The LocalServer32 command line registered for clsid, if any.
[[nodiscard]] std::optional<std::wstring> toast_com_server_command(const GUID& clsid);

/// @brief Registry identity for an unpackaged AppUserModelID (HKCU\Software\Classes\AppUserModelId\<id>),
/// the shortcut-free way to make toasts show the app's name and icon and route clicks to activator_clsid.
struct AppIdRegistration {
    std::wstring app_user_model_id;
    std::wstring display_name;
    std::wstring icon_path;             ///< .png/.ico shown on toasts and in Settings; optional
    std::wstring icon_background_color; ///< "FFRRGGBB"; optional
    std::optional<GUID> activator_clsid;
};
bool register_app_user_model_id(const AppIdRegistration& registration);
bool unregister_app_user_model_id(std::wstring_view app_id);
[[nodiscard]] bool is_app_user_model_id_registered(std::wstring_view app_id);

struct StartMenuShortcut {
    std::wstring name;   ///< file name without .lnk
    std::wstring folder; ///< optional subfolder of the Start menu "Programs" folder
    std::wstring app_user_model_id;
    std::optional<GUID> toast_activator_clsid;
    std::wstring target; ///< empty = this executable
    std::wstring arguments;
    std::wstring description;
    std::wstring icon_path;
    int icon_index = 0;
    std::wstring working_directory;
};

struct ShortcutInfo {
    std::wstring target;
    std::wstring arguments;
    std::wstring app_user_model_id;
    std::optional<GUID> toast_activator_clsid;
};

/// @brief %APPDATA%\Microsoft\Windows\Start Menu\Programs\[folder\]name.lnk
[[nodiscard]] std::optional<std::wstring> start_menu_shortcut_path(std::wstring_view name, std::wstring_view folder = {});
/// @brief Creates (or overwrites) the per-user Start-menu shortcut carrying System.AppUserModel.ID and
/// System.AppUserModel.ToastActivatorCLSID. Any thread.
bool create_start_menu_shortcut(const StartMenuShortcut& shortcut);
/// @brief Deletes the shortcut, and its folder when that becomes empty.
bool remove_start_menu_shortcut(std::wstring_view name, std::wstring_view folder = {});
[[nodiscard]] std::optional<ShortcutInfo> read_shortcut(std::wstring_view lnk_path);

} // namespace boson::shell
