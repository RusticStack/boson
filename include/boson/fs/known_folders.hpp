#pragma once

#include <boson/fs/install_transaction.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::fs {

enum class KnownFolder : std::uint8_t {
    Documents,      ///< follows Known Folder Move / OneDrive redirection (SHGetKnownFolderPath)
    Downloads,
    Desktop,
    Pictures,
    Music,
    Videos,
    SavedGames,
    LocalAppData,
    RoamingAppData,
    ProgramData,
    Profile,        ///< %USERPROFILE%
    Temp,           ///< GetTempPath2W / GetTempPathW
};

/// @brief Current location of a known folder (resolved through redirection, not created).
[[nodiscard]] FsResult<std::filesystem::path> known_folder(KnownFolder folder);

/// @brief OneDrive roots of the current user (personal + business), from the OneDrive* environment
/// variables and HKCU\Software\Microsoft\OneDrive\Accounts\*\UserFolder. Existing directories only.
[[nodiscard]] std::vector<std::filesystem::path> onedrive_roots();
/// @brief true when path is inside one of onedrive_roots() (case-insensitive prefix on whole components).
[[nodiscard]] bool is_under_onedrive(const std::filesystem::path& path);

/// @brief Every plausible "Documents" folder, best first and de-duplicated: the redirected known folder,
/// %USERPROFILE%\Documents, then <OneDrive root>\Documents (and the localized "Documentos"/"Dokumente"
/// variants are not guessed - only folders that exist are returned). Useful to find game data folders
/// ("Documents\Elder Scrolls Online") on machines where OneDrive moved Documents.
[[nodiscard]] std::vector<std::filesystem::path> document_folder_candidates();

/// @brief A uniquely named directory "<Temp>/<prefix>-<pid>-<random>" removed (recursively) on
/// destruction. Move-only.
class TempDir {
public:
    [[nodiscard]] static FsResult<TempDir> create(std::string_view prefix = "boson");
    /// @brief Same, under parent instead of %TEMP%.
    [[nodiscard]] static FsResult<TempDir> create_in(const std::filesystem::path& parent, std::string_view prefix = "boson");

    TempDir(TempDir&& other) noexcept;
    TempDir& operator=(TempDir&& other) noexcept;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    /// @brief Keeps the directory (no removal on destruction) and returns its path.
    std::filesystem::path release() noexcept;

private:
    explicit TempDir(std::filesystem::path path) noexcept : m_path(std::move(path)) {}
    std::filesystem::path m_path;
};

/// @brief Startup cleanup: removes "<parent>/<prefix>-<pid>-*" directories whose process is gone (or
/// older than max_age regardless of pid reuse). parent defaults to %TEMP%. Returns how many were removed.
std::size_t cleanup_stale_temp_dirs(
    std::string_view prefix = "boson",
    std::chrono::hours max_age = std::chrono::hours{24 * 7},
    const std::filesystem::path& parent = {});

/// @brief true when a process with this id is running (OpenProcess + GetExitCodeProcess).
[[nodiscard]] bool process_alive(std::uint32_t pid) noexcept;

} // namespace boson::fs
