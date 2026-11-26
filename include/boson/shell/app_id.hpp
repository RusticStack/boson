#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace boson::shell {

/// @brief true for a usable AppUserModelID: 1..128 characters and no spaces (Microsoft's documented rules;
/// the recommended form is "Company.Product[.SubProduct][.Version]").
[[nodiscard]] bool is_valid_app_user_model_id(std::wstring_view app_id) noexcept;

/// @brief SetCurrentProcessExplicitAppUserModelID. Call at the start of wWinMain, before any window is
/// created: taskbar grouping, jump lists and toasts all key off this ID.
bool set_app_user_model_id(std::wstring_view app_id) noexcept;

/// @brief The explicit AppUserModelID of this process, or std::nullopt when none was set.
[[nodiscard]] std::optional<std::wstring> app_user_model_id();

/// @brief Per-window AppUserModelID (PKEY_AppUserModel_ID on the window property store), which groups
/// that window under a different taskbar button. An empty id removes the per-window value.
bool set_window_app_user_model_id(HWND hwnd, std::wstring_view app_id) noexcept;

} // namespace boson::shell
