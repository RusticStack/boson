#include <boson/crypto/hash.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace boson::crypto {

namespace {

BCRYPT_ALG_HANDLE pseudo_handle(HashAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Md5:
        return BCRYPT_MD5_ALG_HANDLE;
    case HashAlgorithm::Sha1:
        return BCRYPT_SHA1_ALG_HANDLE;
    case HashAlgorithm::Sha256:
        return BCRYPT_SHA256_ALG_HANDLE;
    case HashAlgorithm::Sha384:
        return BCRYPT_SHA384_ALG_HANDLE;
    case HashAlgorithm::Sha512:
        return BCRYPT_SHA512_ALG_HANDLE;
    }
    return nullptr;
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

} // namespace

std::string Digest::hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(length) * 2, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
    }
    return out;
}

bool Digest::matches_hex(std::string_view hex) const noexcept {
    if (hex.size() != static_cast<std::size_t>(length) * 2) {
        return false;
    }
    unsigned diff = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const int hi = hex_value(hex[2 * i]);
        const int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        diff |= static_cast<unsigned>(bytes[i] ^ static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return diff == 0;
}

std::size_t digest_size(HashAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Md5:
        return 16;
    case HashAlgorithm::Sha1:
        return 20;
    case HashAlgorithm::Sha256:
        return 32;
    case HashAlgorithm::Sha384:
        return 48;
    case HashAlgorithm::Sha512:
        return 64;
    }
    return 0;
}

std::string_view to_string(HashAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case HashAlgorithm::Md5:
        return "md5";
    case HashAlgorithm::Sha1:
        return "sha1";
    case HashAlgorithm::Sha256:
        return "sha256";
    case HashAlgorithm::Sha384:
        return "sha384";
    case HashAlgorithm::Sha512:
        return "sha512";
    }
    return "unknown";
}

std::optional<HashAlgorithm> parse_hash_algorithm(std::string_view name) noexcept {
    std::array<char, 16> buf{};
    std::size_t n = 0;
    for (const char c : name) {
        if (c == '-' || c == '_') {
            continue;
        }
        if (n >= buf.size()) {
            return std::nullopt;
        }
        buf[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view norm{buf.data(), n};
    if (norm == "md5") return HashAlgorithm::Md5;
    if (norm == "sha1") return HashAlgorithm::Sha1;
    if (norm == "sha256") return HashAlgorithm::Sha256;
    if (norm == "sha384") return HashAlgorithm::Sha384;
    if (norm == "sha512") return HashAlgorithm::Sha512;
    return std::nullopt;
}

Hasher::Hasher(HashAlgorithm algorithm) noexcept : m_algorithm(algorithm) {
    BCRYPT_HASH_HANDLE handle = nullptr;
    const NTSTATUS status = BCryptCreateHash(
        pseudo_handle(algorithm),
        &handle,
        m_object.data(),
        static_cast<ULONG>(m_object.size()),
        nullptr,
        0,
        BCRYPT_HASH_REUSABLE_FLAG);
    if (BCRYPT_SUCCESS(status)) {
        m_hash = handle;
    }
}

Hasher::~Hasher() {
    if (m_hash != nullptr) {
        BCryptDestroyHash(m_hash);
    }
}

bool Hasher::update(std::span<const std::byte> data) noexcept {
    if (m_hash == nullptr) {
        return false;
    }
    const auto* ptr = reinterpret_cast<const UCHAR*>(data.data());
    std::size_t left = data.size();
    while (left > 0) {
        const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(left, 0x40000000u));
        // BCryptHashData takes a non-const pointer but does not write through it.
        if (!BCRYPT_SUCCESS(BCryptHashData(m_hash, const_cast<UCHAR*>(ptr), chunk, 0))) {
            return false;
        }
        ptr += chunk;
        left -= chunk;
    }
    return true;
}

bool Hasher::update(std::span<const std::uint8_t> data) noexcept {
    return update(std::as_bytes(data));
}

bool Hasher::update(std::string_view data) noexcept {
    return update(std::as_bytes(std::span<const char>(data.data(), data.size())));
}

std::optional<Digest> Hasher::finish() noexcept {
    if (m_hash == nullptr) {
        return std::nullopt;
    }
    Digest out;
    out.length = static_cast<std::uint8_t>(digest_size(m_algorithm));
    if (!BCRYPT_SUCCESS(BCryptFinishHash(m_hash, out.bytes.data(), out.length, 0))) {
        return std::nullopt;
    }
    return out;
}

std::optional<Digest> hash(HashAlgorithm algorithm, std::span<const std::byte> data) noexcept {
    Hasher hasher(algorithm);
    if (!hasher.update(data)) {
        return std::nullopt;
    }
    return hasher.finish();
}

std::optional<Digest> hash(HashAlgorithm algorithm, std::string_view data) noexcept {
    return hash(algorithm, std::as_bytes(std::span<const char>(data.data(), data.size())));
}

std::optional<Digest> hash_file(HashAlgorithm algorithm, const std::filesystem::path& path, std::stop_token stop) {
    const std::unique_ptr<void, HandleCloser> file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE || file.get() == nullptr) {
        return std::nullopt;
    }
    Hasher hasher(algorithm);
    if (!hasher.valid()) {
        return std::nullopt;
    }
    std::vector<std::byte> buffer(1u << 20);
    while (true) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            return std::nullopt;
        }
        if (read == 0) {
            break;
        }
        if (!hasher.update(std::span<const std::byte>(buffer.data(), read))) {
            return std::nullopt;
        }
    }
    return hasher.finish();
}

} // namespace boson::crypto
