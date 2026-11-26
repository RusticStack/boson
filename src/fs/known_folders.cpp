#include <boson/fs/known_folders.hpp>

#include <boson/core/result.hpp>
#include <boson/shipping/crypto.hpp>

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>

namespace boson::fs {

namespace {

struct CoTaskFree {
    void operator()(wchar_t* p) const noexcept { CoTaskMemFree(p); }
};

struct KeyCloser {
    void operator()(HKEY key) const noexcept {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }
};
using UniqueKey = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

std::optional<std::filesystem::path> env_path(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD len = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0 || len >= std::size(buffer)) {
        return std::nullopt;
    }
    return std::filesystem::path{std::wstring_view{buffer, len}};
}

bool is_dir(const std::filesystem::path& p) {
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring normalized_key(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().native();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (!s.empty()) {
        CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
    }
    return s;
}

void push_unique(std::vector<std::filesystem::path>& out, const std::filesystem::path& p) {
    if (p.empty() || !is_dir(p)) {
        return;
    }
    const std::wstring key = normalized_key(p);
    for (const auto& existing : out) {
        if (normalized_key(existing) == key) {
            return;
        }
    }
    out.push_back(p.lexically_normal());
}

} // namespace

FsResult<std::filesystem::path> known_folder(KnownFolder folder) {
    if (folder == KnownFolder::Temp) {
        wchar_t buffer[MAX_PATH + 1]{};
        const DWORD len = GetTempPathW(MAX_PATH + 1, buffer);
        if (len == 0) {
            return std::unexpected(security::last_win32_error("GetTempPathW"));
        }
        return std::filesystem::path{std::wstring_view{buffer, len}};
    }
    const KNOWNFOLDERID* id = nullptr;
    switch (folder) {
    case KnownFolder::Documents: id = &FOLDERID_Documents; break;
    case KnownFolder::Downloads: id = &FOLDERID_Downloads; break;
    case KnownFolder::Desktop: id = &FOLDERID_Desktop; break;
    case KnownFolder::Pictures: id = &FOLDERID_Pictures; break;
    case KnownFolder::Music: id = &FOLDERID_Music; break;
    case KnownFolder::Videos: id = &FOLDERID_Videos; break;
    case KnownFolder::SavedGames: id = &FOLDERID_SavedGames; break;
    case KnownFolder::LocalAppData: id = &FOLDERID_LocalAppData; break;
    case KnownFolder::RoamingAppData: id = &FOLDERID_RoamingAppData; break;
    case KnownFolder::ProgramData: id = &FOLDERID_ProgramData; break;
    case KnownFolder::Profile: id = &FOLDERID_Profile; break;
    case KnownFolder::Temp: break;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    const std::unique_ptr<wchar_t, CoTaskFree> owned(raw);
    if (FAILED(hr) || raw == nullptr) {
        return std::unexpected(security::make_win32_error(static_cast<std::uint32_t>(HRESULT_CODE(hr)), "SHGetKnownFolderPath"));
    }
    return std::filesystem::path{raw};
}

std::vector<std::filesystem::path> onedrive_roots() {
    std::vector<std::filesystem::path> out;
    for (const wchar_t* name : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        if (const auto p = env_path(name)) {
            push_unique(out, *p);
        }
    }
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0, KEY_READ, &raw) == ERROR_SUCCESS) {
        const UniqueKey accounts(raw);
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256]{};
            DWORD sub_len = static_cast<DWORD>(std::size(sub));
            if (RegEnumKeyExW(accounts.get(), i, sub, &sub_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            wchar_t value[MAX_PATH * 2]{};
            DWORD value_bytes = sizeof(value);
            if (RegGetValueW(accounts.get(), sub, L"UserFolder", RRF_RT_REG_SZ, nullptr, value, &value_bytes) == ERROR_SUCCESS) {
                push_unique(out, std::filesystem::path{value});
            }
        }
    }
    return out;
}

bool is_under_onedrive(const std::filesystem::path& path) {
    const std::wstring key = normalized_key(path);
    for (const auto& root : onedrive_roots()) {
        const std::wstring rk = normalized_key(root);
        if (key == rk || (key.size() > rk.size() && key.starts_with(rk) && key[rk.size()] == L'\\')) {
            return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> document_folder_candidates() {
    std::vector<std::filesystem::path> out;
    if (auto docs = known_folder(KnownFolder::Documents)) {
        push_unique(out, *docs);
    }
    if (auto profile = known_folder(KnownFolder::Profile)) {
        push_unique(out, *profile / L"Documents");
    }
    for (const auto& root : onedrive_roots()) {
        push_unique(out, root / L"Documents");
    }
    return out;
}

bool process_alive(std::uint32_t pid) noexcept {
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED; // exists, but belongs to someone else
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

FsResult<TempDir> TempDir::create(std::string_view prefix) {
    auto temp = known_folder(KnownFolder::Temp);
    if (!temp) {
        return std::unexpected(std::move(temp.error()));
    }
    return create_in(*temp, prefix);
}

FsResult<TempDir> TempDir::create_in(const std::filesystem::path& parent, std::string_view prefix) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto rnd = shipping::random_bytes(6);
        const std::string suffix = rnd ? shipping::to_hex(*rnd) : std::to_string(GetTickCount64() + static_cast<unsigned>(attempt));
        std::filesystem::path dir = parent / core::utf8_to_wide(std::string(prefix) + "-" + std::to_string(GetCurrentProcessId()) + "-" + suffix);
        if (CreateDirectoryW(dir.c_str(), nullptr)) {
            return TempDir(std::move(dir));
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return std::unexpected(security::last_win32_error("cannot create temp directory"));
        }
    }
    return std::unexpected(FsError{.code = ERROR_ALREADY_EXISTS, .message = "cannot create a unique temp directory"});
}

TempDir::TempDir(TempDir&& other) noexcept : m_path(std::move(other.m_path)) { other.m_path.clear(); }

TempDir& TempDir::operator=(TempDir&& other) noexcept {
    if (this != &other) {
        if (!m_path.empty()) {
            (void)remove_tree(m_path);
        }
        m_path = std::move(other.m_path);
        other.m_path.clear();
    }
    return *this;
}

TempDir::~TempDir() {
    if (!m_path.empty()) {
        (void)remove_tree(m_path);
    }
}

std::filesystem::path TempDir::release() noexcept {
    std::filesystem::path out = std::move(m_path);
    m_path.clear();
    return out;
}

std::size_t cleanup_stale_temp_dirs(std::string_view prefix, std::chrono::hours max_age, const std::filesystem::path& parent_in) {
    std::filesystem::path parent = parent_in;
    if (parent.empty()) {
        auto temp = known_folder(KnownFolder::Temp);
        if (!temp) {
            return 0;
        }
        parent = *temp;
    }
    const std::wstring head = core::utf8_to_wide(std::string(prefix) + "-");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::size_t removed = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        const std::wstring name = entry.path().filename().native();
        if (!name.starts_with(head) || !entry.is_directory(ec)) {
            continue;
        }
        const std::wstring rest = name.substr(head.size());
        const auto dash = rest.find(L'-');
        if (dash == std::wstring::npos || dash == 0) {
            continue;
        }
        std::uint32_t pid = 0;
        bool numeric = true;
        for (std::size_t i = 0; i < dash; ++i) {
            numeric = numeric && rest[i] >= L'0' && rest[i] <= L'9';
            pid = pid * 10 + static_cast<std::uint32_t>(rest[i] - L'0');
        }
        if (!numeric || pid == GetCurrentProcessId()) {
            continue;
        }
        const auto modified = entry.last_write_time(ec);
        const bool too_old = !ec && now - modified > max_age;
        if (!process_alive(pid) || too_old) {
            if (remove_tree(entry.path())) {
                ++removed;
            }
        }
    }
    return removed;
}

} // namespace boson::fs
