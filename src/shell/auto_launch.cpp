#include <boson/shell/auto_launch.hpp>

#include <boson/app/lifecycle.hpp>

#include <shellapi.h>

#include <array>
#include <optional>

namespace boson::shell {

namespace {

bool valid_name(std::wstring_view name) noexcept {
    return !name.empty() && name.size() < 255 && name.find(L'\\') == std::wstring_view::npos;
}

std::optional<std::wstring> read_run_value(const std::wstring& name) {
    const std::wstring key(kRunKey);
    DWORD bytes = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

/// StartupApproved: REG_BINARY, 12 bytes; first byte even = enabled, odd = disabled (then a FILETIME).
std::optional<bool> read_approved(const std::wstring& name) {
    const std::wstring key(kStartupApprovedKey);
    std::array<BYTE, 32> data{};
    DWORD bytes = static_cast<DWORD>(data.size());
    if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_BINARY, nullptr, data.data(), &bytes) != ERROR_SUCCESS ||
        bytes == 0) {
        return std::nullopt;
    }
    return (data[0] & 1) == 0;
}

bool write_approved_enabled(const std::wstring& name) {
    HKEY key = nullptr;
    const std::wstring path(kStartupApprovedKey);
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const std::array<BYTE, 12> enabled{0x02};
    const bool ok = ::RegSetValueExW(key, name.c_str(), 0, REG_BINARY, enabled.data(), static_cast<DWORD>(enabled.size())) == ERROR_SUCCESS;
    ::RegCloseKey(key);
    return ok;
}

bool delete_value(std::wstring_view key_path, const std::wstring& name) {
    const std::wstring path(key_path);
    const LSTATUS status = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, path.c_str(), name.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

} // namespace

bool enable_auto_launch(const AutoLaunchOptions& options) {
    if (!valid_name(options.name)) {
        return false;
    }
    const std::wstring exe = options.executable.empty() ? app::executable_path() : options.executable;
    if (exe.empty()) {
        return false;
    }
    const std::wstring command = app::build_command_line(exe, options.arguments);

    HKEY key = nullptr;
    const std::wstring path(kRunKey);
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok = ::RegSetValueExW(key, options.name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                     static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    ::RegCloseKey(key);
    if (!ok) {
        return false;
    }
    const auto approved = read_approved(options.name);
    if (approved.has_value() && !*approved && options.override_user_disabled) {
        return write_approved_enabled(options.name);
    }
    return true;
}

bool disable_auto_launch(std::wstring_view name) {
    if (!valid_name(name)) {
        return false;
    }
    const std::wstring value(name);
    const bool run = delete_value(kRunKey, value);
    const bool approved = delete_value(kStartupApprovedKey, value);
    return run && approved;
}

AutoLaunchStatus auto_launch_status(std::wstring_view name) {
    AutoLaunchStatus status;
    if (!valid_name(name)) {
        return status;
    }
    const std::wstring value(name);
    const auto command = read_run_value(value);
    if (!command.has_value()) {
        return status;
    }
    status.registered = true;
    status.command = *command;
    status.disabled_by_user = !read_approved(value).value_or(true);
    status.enabled = !status.disabled_by_user;

    int argc = 0;
    LPWSTR* argv = command->empty() ? nullptr : ::CommandLineToArgvW(command->c_str(), &argc);
    if (argv != nullptr) {
        for (int i = 0; i < argc; ++i) {
            if (i == 0) {
                status.executable = argv[i];
            } else {
                status.arguments.emplace_back(argv[i]);
            }
        }
        ::LocalFree(argv);
    }
    return status;
}

} // namespace boson::shell
