#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boson/security/common.hpp>

namespace boson::security {

enum class ProtectionScope {
    CurrentUser,  ///< only this Windows user can decrypt (default, like Electron safeStorage)
    LocalMachine, ///< any user/process on this machine can decrypt (CRYPTPROTECT_LOCAL_MACHINE)
};

/// @brief Electron-style safeStorage on top of DPAPI (CryptProtectData / CryptUnprotectData with
/// CRYPTPROTECT_UI_FORBIDDEN). Stateless and thread-safe; blocking only for the crypto itself.
class SafeStorage {
public:
    /// @brief entropy is an optional app secret mixed into the key; it must be supplied again to decrypt.
    explicit SafeStorage(std::string entropy = {}, ProtectionScope scope = ProtectionScope::CurrentUser);

    /// @brief true when DPAPI works for this user (tested with a one-time round trip, cached).
    [[nodiscard]] static bool is_available() noexcept;

    [[nodiscard]] SecurityResult<std::vector<std::byte>> encrypt(std::span<const std::byte> plain) const;
    [[nodiscard]] SecurityResult<std::vector<std::byte>> decrypt(std::span<const std::byte> cipher) const;

    [[nodiscard]] SecurityResult<std::vector<std::byte>> encrypt_string(std::string_view plain) const;
    /// @brief Fails with ERROR_INVALID_DATA when the decrypted bytes are not UTF-8.
    [[nodiscard]] SecurityResult<std::string> decrypt_string(std::span<const std::byte> cipher) const;

    [[nodiscard]] ProtectionScope scope() const noexcept { return m_scope; }

private:
    std::string m_entropy;
    ProtectionScope m_scope;
};

} // namespace boson::security
