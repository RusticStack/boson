#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace boson::shipping {

using Bytes = std::vector<std::uint8_t>;
using Sha256Digest = std::array<std::uint8_t, 32>;

/// @brief Incremental SHA-256 over Windows CNG (BCryptCreateHash). Not copyable; one thread at a time.
class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) = delete;
    Sha256& operator=(Sha256&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_hash != nullptr; }
    bool update(std::span<const std::uint8_t> data) noexcept;
    bool update(std::string_view data) noexcept;
    /// @brief Finishes the hash; the object cannot be updated afterwards.
    [[nodiscard]] std::optional<Sha256Digest> finish() noexcept;

private:
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    std::vector<std::uint8_t> m_object;
};

[[nodiscard]] std::optional<Sha256Digest> sha256(std::span<const std::uint8_t> data);
[[nodiscard]] std::optional<Sha256Digest> sha256(std::string_view data);
/// @brief Streams the file in 1 MiB chunks; std::nullopt on I/O failure or when stop is requested.
[[nodiscard]] std::optional<Sha256Digest> sha256_file(const std::filesystem::path& path, std::stop_token stop = {});

/// @brief Lower-case hex.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> data);
/// @brief Accepts upper or lower case; std::nullopt for odd length or non-hex characters.
[[nodiscard]] std::optional<Bytes> from_hex(std::string_view hex);
/// @brief Case-insensitive comparison of two hex strings of the same digest.
[[nodiscard]] bool hex_equals(std::string_view a, std::string_view b) noexcept;

/// @brief Standard base64 (RFC 4648, with padding), no line breaks.
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> data);
/// @brief Standard base64; surrounding whitespace is ignored. std::nullopt when malformed.
[[nodiscard]] std::optional<Bytes> base64_decode(std::string_view text);

/// @brief ECDSA P-256 key material in the raw CNG layout:
/// public_key = X || Y (64 bytes), private_key = X || Y || d (96 bytes).
struct EcdsaP256KeyPair {
    Bytes public_key;
    Bytes private_key;
};

inline constexpr std::size_t kEcdsaP256PublicKeySize = 64;
inline constexpr std::size_t kEcdsaP256PrivateKeySize = 96;
inline constexpr std::size_t kEcdsaP256SignatureSize = 64; ///< r || s, big-endian (CNG / IEEE P1363 form).

/// @brief Generates a new key pair with BCryptGenerateKeyPair.
[[nodiscard]] std::optional<EcdsaP256KeyPair> ecdsa_p256_generate();
/// @brief SHA-256(message) signed with BCryptSignHash. Returns the 64-byte r || s signature.
[[nodiscard]] std::optional<Bytes> ecdsa_p256_sign(std::span<const std::uint8_t> private_key, std::span<const std::uint8_t> message);
/// @brief Verifies a 64-byte r || s signature over SHA-256(message) with BCryptVerifySignature.
[[nodiscard]] bool ecdsa_p256_verify(
    std::span<const std::uint8_t> public_key,
    std::span<const std::uint8_t> message,
    std::span<const std::uint8_t> signature);
/// @brief Same, for an already computed SHA-256 digest.
[[nodiscard]] bool ecdsa_p256_verify_digest(
    std::span<const std::uint8_t> public_key,
    const Sha256Digest& digest,
    std::span<const std::uint8_t> signature);

[[nodiscard]] inline std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

/// @brief Cryptographically random bytes (BCryptGenRandom, system-preferred RNG).
[[nodiscard]] std::optional<Bytes> random_bytes(std::size_t count);

} // namespace boson::shipping
