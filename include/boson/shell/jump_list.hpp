#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace boson::shell {

/// @brief One jump-list entry, written as an IShellLink. Empty program = the running executable.
struct JumpListItem {
    std::wstring title;
    std::wstring description; ///< tooltip
    std::wstring program;
    std::wstring arguments;
    std::wstring working_directory;
    std::wstring icon_path; ///< empty = program
    int icon_index = 0;
    bool separator = false; ///< tasks only: a separator line (every other field is ignored)
};

struct JumpListCategory {
    std::wstring name;
    std::vector<JumpListItem> items;
};

struct JumpListSpec {
    std::vector<JumpListItem> tasks;
    std::vector<JumpListCategory> categories;
    bool show_recent = false;   ///< KDC_RECENT (fed by add_recent_document)
    bool show_frequent = false; ///< KDC_FREQUENT
};

struct JumpListResult {
    bool ok = false;
    HRESULT hr = S_OK;
    std::uint32_t max_slots = 0;            ///< what the shell reports it will show per category
    std::uint32_t skipped_removed = 0;      ///< items the user removed from the list (never re-added)
    std::vector<std::wstring> rejected_categories; ///< categories the shell refused (e.g. tracking disabled)
};

/// @brief Replaces the jump list (ICustomDestinationList). app_id empty = the process AppUserModelID.
/// Safe on any thread (initialises COM for the call when needed). Does disk I/O: keep it off the UI thread.
[[nodiscard]] JumpListResult set_jump_list(const JumpListSpec& spec, std::wstring_view app_id = {});
/// @brief Deletes the custom jump list (tasks and custom categories).
bool clear_jump_list(std::wstring_view app_id = {});

/// @brief SHAddToRecentDocs for path, attributed to app_id (or the process AppUserModelID). Shows up in the
/// jump list "Recent" category and in the user's Recent items.
bool add_recent_document(std::wstring_view path, std::wstring_view app_id = {});
/// @brief Removes this app's Recent/Frequent destinations (IApplicationDestinations::RemoveAllDestinations).
bool clear_recent_documents(std::wstring_view app_id = {});

} // namespace boson::shell
