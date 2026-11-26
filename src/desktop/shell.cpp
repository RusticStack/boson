#include <boson/desktop/shell.hpp>

#include <boson/app/lifecycle.hpp>
#include <boson/core/result.hpp>

#include "desktop_util.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <wrl/client.h>

#include <array>
#include <memory>

namespace boson::desktop {

using Microsoft::WRL::ComPtr;

namespace {

bool is_scheme_char(char ch, bool first) noexcept {
    const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    if (first) {
        return alpha;
    }
    return alpha || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
}

struct PidlDeleter {
    void operator()(ITEMIDLIST* pidl) const noexcept { CoTaskMemFree(pidl); }
};
using UniquePidl = std::unique_ptr<ITEMIDLIST, PidlDeleter>;

ShellResult<std::wstring> existing_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected("path is empty");
    }
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::wstring native = absolute.make_preferred().wstring();
    if (GetFileAttributesW(native.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(detail::last_error_message("path does not exist (" + core::wide_to_utf8(native) + ")"));
    }
    // IFileOperation and SHParseDisplayName reject trailing separators on non-root paths.
    while (native.size() > 3 && (native.back() == L'\\' || native.back() == L'/')) {
        native.pop_back();
    }
    return native;
}

ShellResult<void> shell_execute(const std::wstring& target, std::string_view what) {
    const detail::ScopedCom com;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = nullptr; // default verb ("open" for almost every handler)
    info.lpFile = target.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        return std::unexpected(detail::last_error_message(std::string("ShellExecuteExW(") + std::string(what) + ")"));
    }
    return {};
}

} // namespace

std::optional<std::string> url_scheme(std::string_view url) {
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        if (!is_scheme_char(url[i], i == 0)) {
            return std::nullopt;
        }
    }
    if (colon == 1 && url.size() > 2 && (url[2] == '\\' || url[2] == '/')) {
        return std::nullopt; // "C:\..." / "C:/..." drive path
    }
    return detail::ascii_lower(url.substr(0, colon));
}

ShellResult<void> validate_external_url(std::string_view url, const OpenExternalOptions& options) {
    if (url.empty()) {
        return std::unexpected("URL is empty");
    }
    if (url.size() > 32 * 1024) {
        return std::unexpected("URL is too long");
    }
    for (const char ch : url) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7F || ch == ' ' || ch == '"') {
            return std::unexpected("URL contains whitespace, quotes or control characters; percent-encode them");
        }
    }
    const auto scheme = url_scheme(url);
    if (!scheme) {
        return std::unexpected("URL has no scheme");
    }
    bool allowed = false;
    for (const auto& candidate : options.allowed_schemes) {
        if (detail::iequals(candidate, *scheme)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        return std::unexpected("URL scheme '" + *scheme + "' is not in the allowed list");
    }
    if (*scheme == "http" || *scheme == "https") {
        const std::string_view rest = url.substr(scheme->size() + 1);
        if (!rest.starts_with("//") || rest.size() == 2 || rest[2] == '/' || rest[2] == '?' || rest[2] == '#') {
            return std::unexpected("http(s) URL has no host");
        }
    }
    return {};
}

ShellResult<void> open_external(std::string_view url, const OpenExternalOptions& options) {
    if (auto valid = validate_external_url(url, options); !valid) {
        return valid;
    }
    const std::wstring wide = core::utf8_to_wide(url);
    if (wide.empty()) {
        return std::unexpected("URL is not valid UTF-8");
    }
    return shell_execute(wide, "open_external");
}

ShellResult<void> open_path(const std::filesystem::path& path) {
    auto native = existing_path(path);
    if (!native) {
        return std::unexpected(native.error());
    }
    return shell_execute(*native, "open_path");
}

ShellResult<void> show_item_in_folder(const std::filesystem::path& path) {
    auto native = existing_path(path);
    if (!native) {
        return std::unexpected(native.error());
    }
    const detail::ScopedCom com;
    if (!com.ok()) {
        return std::unexpected("COM is unavailable on this thread");
    }
    ITEMIDLIST* raw = nullptr;
    if (auto error = detail::hr_error(SHParseDisplayName(native->c_str(), nullptr, &raw, 0, nullptr), "SHParseDisplayName")) {
        return std::unexpected(*error);
    }
    const UniquePidl pidl(raw);
    if (auto error = detail::hr_error(SHOpenFolderAndSelectItems(pidl.get(), 0, nullptr, 0), "SHOpenFolderAndSelectItems")) {
        return std::unexpected(*error);
    }
    return {};
}

ShellResult<void> move_to_trash(const std::filesystem::path& path) {
    auto native = existing_path(path);
    if (!native) {
        return std::unexpected(native.error());
    }
    const detail::ScopedCom com;
    if (!com.ok()) {
        return std::unexpected("COM is unavailable on this thread");
    }
    ComPtr<IFileOperation> operation;
    if (auto error = detail::hr_error(
            CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation)), "CoCreateInstance(CLSID_FileOperation)")) {
        return std::unexpected(*error);
    }
    const DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE;
    if (auto error = detail::hr_error(operation->SetOperationFlags(flags), "IFileOperation::SetOperationFlags")) {
        return std::unexpected(*error);
    }
    ComPtr<IShellItem> item;
    if (auto error = detail::hr_error(SHCreateItemFromParsingName(native->c_str(), nullptr, IID_PPV_ARGS(&item)), "SHCreateItemFromParsingName")) {
        return std::unexpected(*error);
    }
    if (auto error = detail::hr_error(operation->DeleteItem(item.Get(), nullptr), "IFileOperation::DeleteItem")) {
        return std::unexpected(*error);
    }
    if (auto error = detail::hr_error(operation->PerformOperations(), "IFileOperation::PerformOperations")) {
        return std::unexpected(*error);
    }
    BOOL aborted = FALSE;
    if (SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted) {
        return std::unexpected("moving to the Recycle Bin was aborted");
    }
    if (GetFileAttributesW(native->c_str()) != INVALID_FILE_ATTRIBUTES) {
        return std::unexpected("the item still exists after the delete operation");
    }
    return {};
}

namespace {

struct KnownFolderEntry {
    KnownFolder folder;
    std::string_view name;
    const KNOWNFOLDERID* id;
};

constexpr std::array<KnownFolderEntry, 19> kKnownFolders{{
    {KnownFolder::Home, "home", &FOLDERID_Profile},
    {KnownFolder::Desktop, "desktop", &FOLDERID_Desktop},
    {KnownFolder::Documents, "documents", &FOLDERID_Documents},
    {KnownFolder::Downloads, "downloads", &FOLDERID_Downloads},
    {KnownFolder::Music, "music", &FOLDERID_Music},
    {KnownFolder::Pictures, "pictures", &FOLDERID_Pictures},
    {KnownFolder::Videos, "videos", &FOLDERID_Videos},
    {KnownFolder::AppData, "appData", &FOLDERID_RoamingAppData},
    {KnownFolder::LocalAppData, "localAppData", &FOLDERID_LocalAppData},
    {KnownFolder::Temp, "temp", nullptr},
    {KnownFolder::Recent, "recent", &FOLDERID_Recent},
    {KnownFolder::StartMenu, "startMenu", &FOLDERID_StartMenu},
    {KnownFolder::Programs, "programs", &FOLDERID_Programs},
    {KnownFolder::Startup, "startup", &FOLDERID_Startup},
    {KnownFolder::ProgramFiles, "programFiles", &FOLDERID_ProgramFiles},
    {KnownFolder::Public, "public", &FOLDERID_Public},
    {KnownFolder::SavedGames, "savedGames", &FOLDERID_SavedGames},
    {KnownFolder::Fonts, "fonts", &FOLDERID_Fonts},
    {KnownFolder::Executable, "exe", nullptr},
}};

std::string normalise_folder_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        if (ch != '_' && ch != '-' && ch != ' ') {
            out.push_back(detail::ascii_lower(ch));
        }
    }
    return out;
}

} // namespace

std::optional<KnownFolder> parse_known_folder(std::string_view name) noexcept {
    try {
        const std::string wanted = normalise_folder_name(name);
        for (const auto& entry : kKnownFolders) {
            if (normalise_folder_name(entry.name) == wanted) {
                return entry.folder;
            }
        }
        if (wanted == "roamingappdata") return KnownFolder::AppData;
        if (wanted == "profile" || wanted == "userprofile") return KnownFolder::Home;
        if (wanted == "tmp") return KnownFolder::Temp;
        if (wanted == "executable") return KnownFolder::Executable;
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::string_view known_folder_name(KnownFolder folder) noexcept {
    for (const auto& entry : kKnownFolders) {
        if (entry.folder == folder) {
            return entry.name;
        }
    }
    return {};
}

ShellResult<std::filesystem::path> known_folder(KnownFolder folder) {
    if (folder == KnownFolder::Temp) {
        wchar_t buffer[MAX_PATH + 1]{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
        if (length == 0 || length > MAX_PATH) {
            return std::unexpected(detail::last_error_message("GetTempPathW"));
        }
        std::wstring path{buffer, length};
        while (path.size() > 3 && path.back() == L'\\') {
            path.pop_back();
        }
        return std::filesystem::path(path);
    }
    if (folder == KnownFolder::Executable) {
        const std::wstring exe = app::executable_path();
        if (exe.empty()) {
            return std::unexpected("GetModuleFileNameW failed");
        }
        return std::filesystem::path(exe);
    }
    for (const auto& entry : kKnownFolders) {
        if (entry.folder != folder || entry.id == nullptr) {
            continue;
        }
        PWSTR raw = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(*entry.id, KF_FLAG_DEFAULT, nullptr, &raw);
        std::filesystem::path path;
        if (SUCCEEDED(hr) && raw != nullptr) {
            path = raw;
        }
        CoTaskMemFree(raw);
        if (auto error = detail::hr_error(hr, std::string("SHGetKnownFolderPath(") + std::string(entry.name) + ")")) {
            return std::unexpected(*error);
        }
        return path;
    }
    return std::unexpected("unknown folder");
}

} // namespace boson::desktop
