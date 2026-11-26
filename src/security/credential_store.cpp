#include <boson/security/credential_store.hpp>

#include <boson/core/result.hpp>

#include <windows.h>
#include <wincred.h>

#include <memory>
#include <stdexcept>

namespace boson::security {

namespace {

struct CredFreeDeleter {
    void operator()(void* ptr) const noexcept {
        if (ptr != nullptr) {
            CredFree(ptr);
        }
    }
};

SecurityError invalid_key() {
    return SecurityError{.code = ERROR_INVALID_PARAMETER, .message = "CredentialStore: invalid key (1-200 chars, no '*', no control characters)"};
}

} // namespace

CredentialStore::CredentialStore(std::string app_prefix) : m_prefix(std::move(app_prefix)) {
    if (m_prefix.empty() || m_prefix.find('*') != std::string::npos) {
        throw std::invalid_argument("CredentialStore: app prefix must be non-empty and contain no '*'");
    }
}

bool CredentialStore::is_valid_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 200) {
        return false;
    }
    for (const char ch : key) {
        if (ch == '*' || static_cast<unsigned char>(ch) < 0x20) {
            return false;
        }
    }
    return is_valid_utf8(key);
}

std::wstring CredentialStore::target_for(std::string_view key) const {
    std::string target = m_prefix;
    target.push_back('/');
    target.append(key);
    return core::utf8_to_wide(target);
}

SecurityResult<void> CredentialStore::set(std::string_view key, std::string_view secret, std::string_view username) const {
    if (!is_valid_key(key)) {
        return std::unexpected(invalid_key());
    }
    if (secret.size() > kMaxSecretBytes) {
        return std::unexpected(SecurityError{.code = ERROR_INVALID_PARAMETER, .message = "CredentialStore: secret exceeds 2560 bytes"});
    }
    if (!is_valid_utf8(secret) || !is_valid_utf8(username)) {
        return std::unexpected(SecurityError{.code = ERROR_INVALID_PARAMETER, .message = "CredentialStore: secret/username must be UTF-8"});
    }
    std::wstring target = target_for(key);
    std::wstring user = core::utf8_to_wide(username);
    std::string blob(secret);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = user.empty() ? nullptr : user.data();
    const BOOL ok = CredWriteW(&credential, 0);
    const DWORD error = GetLastError();
    secure_zero(blob.data(), blob.size());
    if (!ok) {
        return std::unexpected(make_win32_error(error, "CredWriteW"));
    }
    return {};
}

SecurityResult<std::optional<StoredCredential>> CredentialStore::get(std::string_view key) const {
    if (!is_valid_key(key)) {
        return std::unexpected(invalid_key());
    }
    const std::wstring target = target_for(key);
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return std::optional<StoredCredential>{};
        }
        return std::unexpected(make_win32_error(error, "CredReadW"));
    }
    const std::unique_ptr<CREDENTIALW, CredFreeDeleter> guard(raw);
    StoredCredential out{.key = std::string(key)};
    if (raw->UserName != nullptr) {
        out.username = core::wide_to_utf8(raw->UserName);
    }
    if (raw->CredentialBlob != nullptr && raw->CredentialBlobSize != 0) {
        out.secret.assign(reinterpret_cast<const char*>(raw->CredentialBlob), raw->CredentialBlobSize);
        secure_zero(raw->CredentialBlob, raw->CredentialBlobSize);
    }
    return std::optional<StoredCredential>(std::move(out));
}

SecurityResult<bool> CredentialStore::remove(std::string_view key) const {
    if (!is_valid_key(key)) {
        return std::unexpected(invalid_key());
    }
    const std::wstring target = target_for(key);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return false;
        }
        return std::unexpected(make_win32_error(error, "CredDeleteW"));
    }
    return true;
}

SecurityResult<std::vector<std::string>> CredentialStore::list() const {
    const std::wstring filter = core::utf8_to_wide(m_prefix + "/*");
    DWORD count = 0;
    PCREDENTIALW* raw = nullptr;
    if (!CredEnumerateW(filter.c_str(), 0, &count, &raw)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return std::vector<std::string>{};
        }
        return std::unexpected(make_win32_error(error, "CredEnumerateW"));
    }
    const std::unique_ptr<PCREDENTIALW, CredFreeDeleter> guard(raw);
    const std::string prefix = m_prefix + "/";
    std::vector<std::string> keys;
    keys.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        const CREDENTIALW* credential = raw[i];
        if (credential == nullptr || credential->Type != CRED_TYPE_GENERIC || credential->TargetName == nullptr) {
            continue;
        }
        std::string target = core::wide_to_utf8(credential->TargetName);
        // The filter is case-insensitive; keep only exact-prefix matches.
        if (target.size() > prefix.size() && target.starts_with(prefix)) {
            keys.push_back(target.substr(prefix.size()));
        }
    }
    return keys;
}

} // namespace boson::security
