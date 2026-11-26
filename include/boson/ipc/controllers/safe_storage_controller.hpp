#pragma once

#include <boson/ipc/export.hpp>
#include <boson/security/common.hpp>
#include <boson/security/safe_storage.hpp>

#include <glaze/glaze.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace boson::ipc {

struct SafeStorageText {
    std::string text;
};

struct SafeStorageData {
    std::string data; ///< base64 (DPAPI blob or raw bytes)
};

struct SafeStorageAvailability {
    bool value = false;
};

/// @brief JS "SafeStorage": DPAPI encryption bound to the current Windows user (Electron safeStorage).
/// Ciphertext travels as base64. Runs on the router's pool thread.
class SafeStorageController {
public:
    BOSON_CONTROLLER("SafeStorage");

    explicit SafeStorageController(security::SafeStorage storage = security::SafeStorage{})
        : m_storage(std::move(storage)) {}

    [[nodiscard]] SafeStorageAvailability is_available() const {
        return SafeStorageAvailability{.value = security::SafeStorage::is_available()};
    }

    [[nodiscard]] SafeStorageData encrypt_string(SafeStorageText req) const {
        auto cipher = m_storage.encrypt_string(req.text);
        security::secure_zero(req.text.data(), req.text.size());
        if (!cipher) {
            throw std::runtime_error(cipher.error().message);
        }
        return SafeStorageData{.data = security::base64_encode(*cipher)};
    }

    [[nodiscard]] SafeStorageText decrypt_string(SafeStorageData req) const {
        auto cipher = security::base64_decode(req.data);
        if (!cipher) {
            throw std::runtime_error(cipher.error().message);
        }
        auto plain = m_storage.decrypt_string(*cipher);
        if (!plain) {
            throw std::runtime_error(plain.error().message);
        }
        return SafeStorageText{.text = std::move(*plain)};
    }

    [[nodiscard]] SafeStorageData encrypt_bytes(SafeStorageData req) const {
        auto plain = security::base64_decode(req.data);
        if (!plain) {
            throw std::runtime_error(plain.error().message);
        }
        auto cipher = m_storage.encrypt(*plain);
        security::secure_zero(plain->data(), plain->size());
        if (!cipher) {
            throw std::runtime_error(cipher.error().message);
        }
        return SafeStorageData{.data = security::base64_encode(*cipher)};
    }

    [[nodiscard]] SafeStorageData decrypt_bytes(SafeStorageData req) const {
        auto cipher = security::base64_decode(req.data);
        if (!cipher) {
            throw std::runtime_error(cipher.error().message);
        }
        auto plain = m_storage.decrypt(*cipher);
        if (!plain) {
            throw std::runtime_error(plain.error().message);
        }
        SafeStorageData out{.data = security::base64_encode(*plain)};
        security::secure_zero(plain->data(), plain->size());
        return out;
    }

private:
    security::SafeStorage m_storage;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::SafeStorageController> {
    using T = boson::ipc::SafeStorageController;
    static constexpr auto value = glz::object(
        "is_available", &T::is_available,
        "encrypt_string", &T::encrypt_string,
        "decrypt_string", &T::decrypt_string,
        "encrypt_bytes", &T::encrypt_bytes,
        "decrypt_bytes", &T::decrypt_bytes);
};
