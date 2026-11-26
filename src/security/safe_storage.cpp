#include <boson/security/safe_storage.hpp>

#include <windows.h>
#include <dpapi.h>

#include <climits>
#include <memory>

namespace boson::security {

namespace {

struct LocalFreeDeleter {
    void operator()(BYTE* ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};

DATA_BLOB blob_of(std::span<const std::byte> bytes) noexcept {
    // DPAPI rejects a null pbData even for zero-length input; point empty blobs at a dummy byte.
    static const BYTE kEmpty = 0;
    // DPAPI takes non-const pointers but does not write through pbData of input blobs.
    return DATA_BLOB{
        .cbData = static_cast<DWORD>(bytes.size()),
        .pbData = const_cast<BYTE*>(bytes.empty() ? &kEmpty : reinterpret_cast<const BYTE*>(bytes.data())),
    };
}

} // namespace

SafeStorage::SafeStorage(std::string entropy, ProtectionScope scope)
    : m_entropy(std::move(entropy)), m_scope(scope) {}

bool SafeStorage::is_available() noexcept {
    static const bool available = []() noexcept {
        try {
            const SafeStorage probe("boson-probe");
            const auto cipher = probe.encrypt_string("boson");
            if (!cipher) {
                return false;
            }
            const auto plain = probe.decrypt_string(*cipher);
            return plain.has_value() && *plain == "boson";
        } catch (...) {
            return false;
        }
    }();
    return available;
}

SecurityResult<std::vector<std::byte>> SafeStorage::encrypt(std::span<const std::byte> plain) const {
    if (plain.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected(SecurityError{.code = ERROR_INVALID_PARAMETER, .message = "SafeStorage: input too large"});
    }
    DATA_BLOB in = blob_of(plain);
    DATA_BLOB entropy = blob_of(as_bytes(m_entropy));
    DATA_BLOB out{};
    DWORD flags = CRYPTPROTECT_UI_FORBIDDEN;
    if (m_scope == ProtectionScope::LocalMachine) {
        flags |= CRYPTPROTECT_LOCAL_MACHINE;
    }
    if (!CryptProtectData(&in, L"boson", m_entropy.empty() ? nullptr : &entropy, nullptr, nullptr, flags, &out)) {
        return std::unexpected(last_win32_error("CryptProtectData"));
    }
    const std::unique_ptr<BYTE, LocalFreeDeleter> guard(out.pbData);
    const auto* begin = reinterpret_cast<const std::byte*>(out.pbData);
    return std::vector<std::byte>(begin, begin + out.cbData);
}

SecurityResult<std::vector<std::byte>> SafeStorage::decrypt(std::span<const std::byte> cipher) const {
    if (cipher.empty() || cipher.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected(SecurityError{.code = ERROR_INVALID_DATA, .message = "SafeStorage: invalid ciphertext"});
    }
    DATA_BLOB in = blob_of(cipher);
    DATA_BLOB entropy = blob_of(as_bytes(m_entropy));
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, m_entropy.empty() ? nullptr : &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::unexpected(last_win32_error("CryptUnprotectData"));
    }
    const std::unique_ptr<BYTE, LocalFreeDeleter> guard(out.pbData);
    if (out.pbData == nullptr || out.cbData == 0) {
        return std::vector<std::byte>{};
    }
    const auto* begin = reinterpret_cast<const std::byte*>(out.pbData);
    std::vector<std::byte> plain(begin, begin + out.cbData);
    secure_zero(out.pbData, out.cbData);
    return plain;
}

SecurityResult<std::vector<std::byte>> SafeStorage::encrypt_string(std::string_view plain) const {
    return encrypt(as_bytes(plain));
}

SecurityResult<std::string> SafeStorage::decrypt_string(std::span<const std::byte> cipher) const {
    auto plain = decrypt(cipher);
    if (!plain) {
        return std::unexpected(std::move(plain.error()));
    }
    std::string text(reinterpret_cast<const char*>(plain->data()), plain->size());
    secure_zero(plain->data(), plain->size());
    if (!is_valid_utf8(text)) {
        secure_zero(text.data(), text.size());
        return std::unexpected(SecurityError{.code = ERROR_INVALID_DATA, .message = "SafeStorage: plaintext is not UTF-8"});
    }
    return text;
}

} // namespace boson::security
