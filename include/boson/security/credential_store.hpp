#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boson/security/common.hpp>

namespace boson::security {

struct StoredCredential {
    std::string key;      ///< the name passed to set(), without the app prefix
    std::string username; ///< may be empty
    std::string secret;   ///< UTF-8
};

/// @brief Windows Credential Manager (CRED_TYPE_GENERIC, CRED_PERSIST_LOCAL_MACHINE) scoped to one app:
/// every target name is "<app_prefix>/<key>". Thread-safe; calls block briefly (do them off the UI thread).
class CredentialStore {
public:
    /// Max secret size Credential Manager accepts (CRED_MAX_CREDENTIAL_BLOB_SIZE).
    static constexpr std::size_t kMaxSecretBytes = 5 * 512;

    /// @brief app_prefix like "com.example.myapp"; must be non-empty and contain no '*'.
    explicit CredentialStore(std::string app_prefix);

    [[nodiscard]] SecurityResult<void> set(std::string_view key, std::string_view secret, std::string_view username = {}) const;
    /// @brief nullopt inside the result when the key does not exist.
    [[nodiscard]] SecurityResult<std::optional<StoredCredential>> get(std::string_view key) const;
    /// @brief true when something was deleted, false when the key did not exist.
    [[nodiscard]] SecurityResult<bool> remove(std::string_view key) const;
    /// @brief Keys (without prefix) of every credential of this app.
    [[nodiscard]] SecurityResult<std::vector<std::string>> list() const;

    [[nodiscard]] const std::string& prefix() const noexcept { return m_prefix; }
    [[nodiscard]] std::wstring target_for(std::string_view key) const;
    [[nodiscard]] static bool is_valid_key(std::string_view key) noexcept;

private:
    std::string m_prefix;
};

} // namespace boson::security
