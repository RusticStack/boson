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

namespace boson::crypto {

/// @brief Hash algorithms backed by Windows CNG pseudo-handles (no BCryptOpenAlgorithmProvider per use).
enum class HashAlgorithm : std::uint8_t {
    Md5,    ///< 16 bytes. Integrity checks against legacy feeds only; not collision resistant.
    Sha1,   ///< 20 bytes. Same caveat as Md5.
    Sha256, ///< 32 bytes.
    Sha384, ///< 48 bytes.
    Sha512, ///< 64 bytes.
};

inline constexpr std::size_t kMaxDigestSize = 64;

/// @brief A digest of up to 64 bytes; size() depends on the algorithm. Trivially copyable, no heap.
struct Digest {
    std::array<std::uint8_t, kMaxDigestSize> bytes{};
    std::uint8_t length = 0;

    [[nodiscard]] std::size_t size() const noexcept { return length; }
    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return {bytes.data(), length}; }
    /// @brief Lower-case hex.
    [[nodiscard]] std::string hex() const;
    /// @brief Case-insensitive comparison with a hex string (constant time over the digest length).
    [[nodiscard]] bool matches_hex(std::string_view hex) const noexcept;
    friend bool operator==(const Digest& a, const Digest& b) noexcept {
        return a.length == b.length && a.bytes == b.bytes;
    }
};

[[nodiscard]] std::size_t digest_size(HashAlgorithm algorithm) noexcept;
[[nodiscard]] std::string_view to_string(HashAlgorithm algorithm) noexcept;
/// @brief "md5", "sha1", "sha-1", "sha256", "sha-256", ... (case-insensitive).
[[nodiscard]] std::optional<HashAlgorithm> parse_hash_algorithm(std::string_view name) noexcept;

/// @brief Incremental hash over CNG. The hash object lives inside this instance (no heap allocation).
/// Not copyable; one thread at a time. finish() resets it so the instance can hash again.
class Hasher {
public:
    explicit Hasher(HashAlgorithm algorithm) noexcept;
    ~Hasher();

    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    Hasher(Hasher&& other) = delete;
    Hasher& operator=(Hasher&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_hash != nullptr; }
    [[nodiscard]] HashAlgorithm algorithm() const noexcept { return m_algorithm; }
    bool update(std::span<const std::byte> data) noexcept;
    bool update(std::span<const std::uint8_t> data) noexcept;
    bool update(std::string_view data) noexcept;
    /// @brief Returns the digest and resets the hasher (CNG reusable hash objects).
    [[nodiscard]] std::optional<Digest> finish() noexcept;

private:
    HashAlgorithm m_algorithm;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    // Large enough for the CNG hash object of every algorithm above (SHA-512 needs ~ 0x200 bytes).
    alignas(16) std::array<std::uint8_t, 1024> m_object{};
};

/// @brief One-shot hash of a buffer.
[[nodiscard]] std::optional<Digest> hash(HashAlgorithm algorithm, std::span<const std::byte> data) noexcept;
[[nodiscard]] std::optional<Digest> hash(HashAlgorithm algorithm, std::string_view data) noexcept;

/// @brief Streams a file (sequential-scan hint, 1 MiB reads). std::nullopt on I/O failure or stop.
/// Blocking: call from a pool thread.
[[nodiscard]] std::optional<Digest> hash_file(
    HashAlgorithm algorithm, const std::filesystem::path& path, std::stop_token stop = {});

} // namespace boson::crypto
