#include <boson/desktop/deep_link.hpp>

#include <boson/core/result.hpp>

#include "desktop_util.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <array>

namespace boson::desktop {

namespace {

constexpr std::array<std::string_view, 22> kReservedSchemes{
    "http", "https", "file", "ftp", "ftps", "mailto", "javascript", "data", "blob", "about", "chrome",
    "edge", "ws", "wss", "res", "shell", "search-ms", "microsoft-edge", "callto", "tel", "news", "telnet",
};

LONG write_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring read_string(HKEY root, const std::wstring& sub_key, const wchar_t* value_name) {
    DWORD size = 0;
    if (RegGetValueW(root, sub_key.c_str(), value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, sub_key.c_str(), value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

bool same_program(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    wchar_t full_a[MAX_PATH * 4]{};
    wchar_t full_b[MAX_PATH * 4]{};
    const std::wstring sa{a};
    const std::wstring sb{b};
    const DWORD la = GetFullPathNameW(sa.c_str(), static_cast<DWORD>(std::size(full_a)), full_a, nullptr);
    const DWORD lb = GetFullPathNameW(sb.c_str(), static_cast<DWORD>(std::size(full_b)), full_b, nullptr);
    const wchar_t* pa = (la != 0 && la < std::size(full_a)) ? full_a : sa.c_str();
    const wchar_t* pb = (lb != 0 && lb < std::size(full_b)) ? full_b : sb.c_str();
    return CompareStringOrdinal(pa, -1, pb, -1, TRUE) == CSTR_EQUAL;
}

std::wstring resolve_executable(std::wstring_view executable) {
    return executable.empty() ? app::executable_path() : std::wstring(executable);
}

} // namespace

bool is_valid_custom_scheme(std::string_view scheme) noexcept {
    if (scheme.size() < 2 || scheme.size() > 64) {
        return false;
    }
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        const char ch = scheme[i];
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool other = (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
        if (!(alpha || (i > 0 && other))) {
            return false;
        }
    }
    for (const auto reserved : kReservedSchemes) {
        if (detail::iequals(reserved, scheme)) {
            return false;
        }
    }
    if (scheme.size() >= 3 && detail::iequals(scheme.substr(0, 3), "ms-")) {
        return false;
    }
    return true;
}

std::wstring command_program(std::wstring_view command_line) {
    while (!command_line.empty() && command_line.front() == L' ') {
        command_line.remove_prefix(1);
    }
    if (command_line.empty()) {
        return {};
    }
    if (command_line.front() == L'"') {
        const std::size_t end = command_line.find(L'"', 1);
        return std::wstring(command_line.substr(1, end == std::wstring_view::npos ? std::wstring_view::npos : end - 1));
    }
    const std::size_t space = command_line.find(L' ');
    return std::wstring(command_line.substr(0, space));
}

std::expected<void, std::string> register_protocol_client(std::string_view scheme, const ProtocolClientOptions& options) {
    if (!is_valid_custom_scheme(scheme)) {
        return std::unexpected("'" + std::string(scheme) + "' is not a valid custom URL scheme");
    }
    const std::wstring exe = resolve_executable(options.executable);
    if (exe.empty()) {
        return std::unexpected("executable path is unknown");
    }
    const std::wstring wscheme = core::utf8_to_wide(detail::ascii_lower(scheme));
    const std::wstring root = L"Software\\Classes\\" + wscheme;

    std::wstring command = app::quote_argument(exe);
    for (const auto& arg : options.arguments) {
        command += L' ';
        command += app::quote_argument(arg);
    }
    command += L" \"%1\"";
    const std::wstring description = L"URL:" + (options.description.empty() ? wscheme : options.description);
    const std::wstring icon = options.icon.empty() ? (app::quote_argument(exe) + L",0") : options.icon;

    HKEY key = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return std::unexpected("RegCreateKeyExW(" + core::wide_to_utf8(root) + "): " + detail::win32_message(static_cast<DWORD>(status)));
    }
    status = write_string(key, nullptr, description);
    if (status == ERROR_SUCCESS) {
        status = write_string(key, L"URL Protocol", L"");
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return std::unexpected("RegSetValueExW: " + detail::win32_message(static_cast<DWORD>(status)));
    }

    const auto set_default = [](const std::wstring& path, const std::wstring& value) -> LONG {
        HKEY sub = nullptr;
        LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &sub, nullptr);
        if (result != ERROR_SUCCESS) {
            return result;
        }
        result = write_string(sub, nullptr, value);
        RegCloseKey(sub);
        return result;
    };
    status = set_default(root + L"\\DefaultIcon", icon);
    if (status == ERROR_SUCCESS) {
        status = set_default(root + L"\\shell\\open\\command", command);
    }
    if (status != ERROR_SUCCESS) {
        RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
        return std::unexpected("writing the protocol command failed: " + detail::win32_message(static_cast<DWORD>(status)));
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return {};
}

std::expected<void, std::string> unregister_protocol_client(std::string_view scheme, std::wstring_view executable, bool force) {
    if (!is_valid_custom_scheme(scheme)) {
        return std::unexpected("'" + std::string(scheme) + "' is not a valid custom URL scheme");
    }
    const std::wstring root = L"Software\\Classes\\" + core::utf8_to_wide(detail::ascii_lower(scheme));
    HKEY probe = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, KEY_READ, &probe) != ERROR_SUCCESS) {
        return {}; // nothing registered for this user
    }
    RegCloseKey(probe);
    if (!force) {
        const std::wstring command = read_string(HKEY_CURRENT_USER, root + L"\\shell\\open\\command", nullptr);
        if (!same_program(command_program(command), resolve_executable(executable))) {
            return std::unexpected("'" + std::string(scheme) + "' is registered to another program (" +
                                   core::wide_to_utf8(command_program(command)) + "); pass force to remove it anyway");
        }
    }
    const LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return std::unexpected("RegDeleteTreeW: " + detail::win32_message(static_cast<DWORD>(status)));
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, root.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return {};
}

std::wstring protocol_handler_command(std::string_view scheme) {
    if (!is_valid_custom_scheme(scheme)) {
        return {};
    }
    const std::wstring wscheme = core::utf8_to_wide(detail::ascii_lower(scheme));
    const std::wstring choice = read_string(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\" + wscheme + L"\\UserChoice",
        L"ProgId");
    const std::wstring prog_id = choice.empty() ? wscheme : choice;
    return read_string(HKEY_CLASSES_ROOT, prog_id + L"\\shell\\open\\command", nullptr);
}

bool is_default_protocol_client(std::string_view scheme, std::wstring_view executable) {
    try {
        const std::wstring command = protocol_handler_command(scheme);
        return !command.empty() && same_program(command_program(command), resolve_executable(executable));
    } catch (...) {
        return false;
    }
}

std::vector<std::string> extract_deep_links(std::span<const std::wstring> args, std::string_view scheme) {
    std::vector<std::string> links;
    const std::wstring prefix = core::utf8_to_wide(scheme) + L":";
    for (const auto& arg : args) {
        if (arg.size() <= prefix.size()) {
            continue;
        }
        if (CompareStringOrdinal(arg.c_str(), static_cast<int>(prefix.size()), prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
            links.push_back(core::wide_to_utf8(arg));
        }
    }
    return links;
}

app::SingleInstance::SecondInstanceFn deep_link_forwarder(
    std::string scheme,
    std::function<void(std::vector<std::string> links)> on_links,
    app::SingleInstance::SecondInstanceFn on_other_launch) {
    return [scheme = std::move(scheme), on_links = std::move(on_links), on_other_launch = std::move(on_other_launch)](
               std::vector<std::wstring> args, std::wstring cwd) {
        auto links = extract_deep_links(args, scheme);
        if (!links.empty()) {
            if (on_links) {
                on_links(std::move(links));
            }
            return;
        }
        if (on_other_launch) {
            on_other_launch(std::move(args), std::move(cwd));
        }
    };
}

} // namespace boson::desktop
