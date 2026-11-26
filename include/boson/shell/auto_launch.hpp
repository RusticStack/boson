#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace boson::shell {

struct AutoLaunchOptions {
    /// Value name under HKCU\...\Run; also the name Task Manager's Startup apps page shows.
    std::wstring name;
    std::wstring executable; ///< empty = this executable
    std::vector<std::wstring> arguments;
    /// When the user disabled the entry in Task Manager / Settings (StartupApproved), re-approve it.
    /// Only do this in response to an explicit user action such as a "Launch at login" toggle.
    bool override_user_disabled = true;
};

struct AutoLaunchStatus {
    bool registered = false;        ///< a Run value exists
    bool disabled_by_user = false;  ///< StartupApproved marks it disabled
    bool enabled = false;           ///< registered && !disabled_by_user: it will start at login
    std::wstring command;           ///< the raw Run value
    std::wstring executable;
    std::vector<std::wstring> arguments;
};

/// @brief HKCU\Software\Microsoft\Windows\CurrentVersion\Run value = quoted executable + quoted args.
bool enable_auto_launch(const AutoLaunchOptions& options);
/// @brief Deletes the Run value and its StartupApproved entry.
bool disable_auto_launch(std::wstring_view name);
[[nodiscard]] AutoLaunchStatus auto_launch_status(std::wstring_view name);
[[nodiscard]] inline bool is_auto_launch_enabled(std::wstring_view name) { return auto_launch_status(name).enabled; }

/// @brief Registry paths (relative to HKEY_CURRENT_USER), exposed for tests and diagnostics.
inline constexpr std::wstring_view kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr std::wstring_view kStartupApprovedKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

} // namespace boson::shell
