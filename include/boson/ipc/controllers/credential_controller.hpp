#pragma once

#include <boson/ipc/export.hpp>
#include <boson/security/credential_store.hpp>

#include <glaze/glaze.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct CredentialSetRequest {
    std::string key;
    std::string secret;
    std::string username;
};

struct CredentialKeyRequest {
    std::string key;
};

struct CredentialGetResponse {
    bool found = false;
    std::string username;
    std::string secret;
};

struct CredentialRemoveResponse {
    bool value = false;
};

struct CredentialListResponse {
    std::vector<std::string> keys;
};

/// @brief JS "Credential": Windows Credential Manager entries under the app's target prefix.
/// Runs on the router's pool thread. Gate it with a capability policy: it hands secrets to JS.
class CredentialController {
public:
    BOSON_CONTROLLER("Credential");

    explicit CredentialController(security::CredentialStore store) : m_store(std::move(store)) {}

    void set(CredentialSetRequest req) const {
        auto result = m_store.set(req.key, req.secret, req.username);
        security::secure_zero(req.secret.data(), req.secret.size());
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
    }

    [[nodiscard]] CredentialGetResponse get(CredentialKeyRequest req) const {
        auto result = m_store.get(req.key);
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
        if (!result->has_value()) {
            return CredentialGetResponse{};
        }
        auto& credential = **result;
        return CredentialGetResponse{
            .found = true,
            .username = std::move(credential.username),
            .secret = std::move(credential.secret),
        };
    }

    [[nodiscard]] CredentialRemoveResponse remove(CredentialKeyRequest req) const {
        auto result = m_store.remove(req.key);
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
        return CredentialRemoveResponse{.value = *result};
    }

    [[nodiscard]] CredentialListResponse list() const {
        auto result = m_store.list();
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
        return CredentialListResponse{.keys = std::move(*result)};
    }

private:
    security::CredentialStore m_store;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::CredentialController> {
    using T = boson::ipc::CredentialController;
    static constexpr auto value = glz::object(
        "set", &T::set,
        "get", &T::get,
        "remove", &T::remove,
        "list", &T::list);
};
