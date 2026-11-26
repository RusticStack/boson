#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::desktop {

template <typename T>
using ShellResult = std::expected<T, std::string>;

/// @brief Lower-cased RFC 3986 scheme of url ("https" for "HTTPS://x"), nullopt when url has none. A
/// single letter followed by ":\" or ":/" is a Windows drive path, not a scheme.
[[nodiscard]] std::optional<std::string> url_scheme(std::string_view url);

struct OpenExternalOptions {
    /// Lower-case schemes open_external() may hand to the shell. Never add "file" or "ms-*" schemes for
    /// URLs that come from web content: they can launch local programs.
    std::vector<std::string> allowed_schemes{"http", "https", "mailto"};
};

/// @brief Checks url against options without launching anything: an allowed scheme, no control
/// characters or spaces, and a host for http(s).
[[nodiscard]] ShellResult<void> validate_external_url(std::string_view url, const OpenExternalOptions& options = {});

/// @brief Opens url with the user's default handler (ShellExecuteExW). Blocks while the shell resolves
/// the handler: call it from a TaskRunner thread.
[[nodiscard]] ShellResult<void> open_external(std::string_view url, const OpenExternalOptions& options = {});

/// @brief Opens a file with its associated application, or a folder in Explorer. The path must exist.
[[nodiscard]] ShellResult<void> open_path(const std::filesystem::path& path);

/// @brief Opens the parent folder in Explorer with the item selected (SHOpenFolderAndSelectItems).
[[nodiscard]] ShellResult<void> show_item_in_folder(const std::filesystem::path& path);

/// @brief Moves a file or folder to the Recycle Bin (IFileOperation + FOFX_RECYCLEONDELETE), silently.
/// On a volume without a Recycle Bin the shell deletes the item permanently, as Explorer does with the
/// confirmation suppressed.
[[nodiscard]] ShellResult<void> move_to_trash(const std::filesystem::path& path);

enum class KnownFolder {
    Home,
    Desktop,
    Documents,
    Downloads,
    Music,
    Pictures,
    Videos,
    AppData,       ///< Roaming application data (%APPDATA%)
    LocalAppData,  ///< %LOCALAPPDATA%
    Temp,          ///< GetTempPathW
    Recent,
    StartMenu,
    Programs,      ///< Start menu "Programs"
    Startup,
    ProgramFiles,
    Public,
    SavedGames,
    Fonts,
    Executable,    ///< The running .exe (a file, not a folder)
};

/// @brief Resolves a known folder (SHGetKnownFolderPath, KF_FLAG_DEFAULT; the folder is not created).
[[nodiscard]] ShellResult<std::filesystem::path> known_folder(KnownFolder folder);

/// @brief Electron-style names, case-insensitive, '_' and '-' ignored: "home", "desktop", "documents",
/// "downloads", "music", "pictures", "videos", "appData", "localAppData", "temp", "recent", "startMenu",
/// "programs", "startup", "programFiles", "public", "savedGames", "fonts", "exe".
[[nodiscard]] std::optional<KnownFolder> parse_known_folder(std::string_view name) noexcept;
[[nodiscard]] std::string_view known_folder_name(KnownFolder folder) noexcept;

} // namespace boson::desktop
