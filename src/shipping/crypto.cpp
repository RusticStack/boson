#include <boson/shipping/crypto.hpp>

#include <boson/core/result.hpp>

#include <wincrypt.h>

#include <cstring>
#include <memory>

namespace boson::shipping {

namespace {

constexpr NTSTATUS kStatusSuccess = 0;

bool nt_check(NTSTATUS status, std::string_view context) noexcept {
    if (status >= 0) {
        return true;
    }
    return core::hr_check(HRESULT_FROM_NT(status), context);
}

struct AlgCloser {
    void operator()(BCRYPT_ALG_HANDLE alg) const noexcept {
        if (alg != nullptr) {
            BCryptCloseAlgorithmProvider(alg, 0);
        }
    }
};
using UniqueAlg = std::unique_ptr<void, AlgCloser>;

struct KeyDestroyer {
    void operator()(BCRYPT_KEY_HANDLE key) const noexcept {
        if (key != nullptr) {
            BCryptDestroyKey(key);
        }
    }
};
using UniqueKey = std::unique_ptr<void, KeyDestroyer>;

UniqueAlg open_ecdsa() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!nt_check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0), "BCryptOpenAlgorithmProvider(ECDSA_P256)")) {
        return {};
    }
    return UniqueAlg(alg);
}

UniqueKey import_key(BCRYPT_ALG_HANDLE alg, std::span<const std::uint8_t> raw, bool is_private) {
    const std::size_t expected = is_private ? kEcdsaP256PrivateKeySize : kEcdsaP256PublicKeySize;
    if (raw.size() != expected) {
        return {};
    }
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + raw.size());
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = is_private ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32;
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), raw.data(), raw.size());

    BCRYPT_KEY_HANDLE key = nullptr;
    const NTSTATUS status = BCryptImportKeyPair(
        alg,
        nullptr,
        is_private ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB,
        &key,
        blob.data(),
        static_cast<ULONG>(blob.size()),
        0);
    SecureZeroMemory(blob.data(), blob.size());
    if (!nt_check(status, "BCryptImportKeyPair")) {
        return {};
    }
    return UniqueKey(key);
}

std::optional<Bytes> export_key(BCRYPT_KEY_HANDLE key, bool is_private) {
    const wchar_t* type = is_private ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB;
    ULONG size = 0;
    if (!nt_check(BCryptExportKey(key, nullptr, type, nullptr, 0, &size, 0), "BCryptExportKey(size)")) {
        return std::nullopt;
    }
    Bytes blob(size);
    if (!nt_check(BCryptExportKey(key, nullptr, type, blob.data(), size, &size, 0), "BCryptExportKey")) {
        return std::nullopt;
    }
    if (size < sizeof(BCRYPT_ECCKEY_BLOB)) {
        return std::nullopt;
    }
    Bytes raw(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.begin() + size);
    SecureZeroMemory(blob.data(), blob.size());
    const std::size_t expected = is_private ? kEcdsaP256PrivateKeySize : kEcdsaP256PublicKeySize;
    if (raw.size() != expected) {
        return std::nullopt;
    }
    return raw;
}

int hex_value(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

} // namespace

// ---- SHA-256 ------------------------------------------------------------------------------------------

Sha256::Sha256() {
    // BCRYPT_SHA256_ALG_HANDLE is a pseudo-handle (Windows 10+): no provider to open or close.
    ULONG object_size = 0;
    ULONG got = 0;
    if (!nt_check(
            BCryptGetProperty(
                BCRYPT_SHA256_ALG_HANDLE,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &got,
                0),
            "BCryptGetProperty(OBJECT_LENGTH)")) {
        return;
    }
    m_object.resize(object_size);
    if (!nt_check(
            BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &m_hash, m_object.data(), object_size, nullptr, 0, 0),
            "BCryptCreateHash(SHA256)")) {
        m_hash = nullptr;
    }
}

Sha256::~Sha256() {
    if (m_hash != nullptr) {
        BCryptDestroyHash(m_hash);
    }
}

bool Sha256::update(std::span<const std::uint8_t> data) noexcept {
    if (m_hash == nullptr) {
        return false;
    }
    // BCryptHashData takes a ULONG length: feed very large spans in pieces.
    constexpr std::size_t kMaxChunk = 0x40000000;
    while (!data.empty()) {
        const std::size_t n = std::min(data.size(), kMaxChunk);
        if (!nt_check(
                BCryptHashData(m_hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(n), 0),
                "BCryptHashData")) {
            return false;
        }
        data = data.subspan(n);
    }
    return true;
}

bool Sha256::update(std::string_view data) noexcept {
    return update(as_bytes(data));
}

std::optional<Sha256Digest> Sha256::finish() noexcept {
    if (m_hash == nullptr) {
        return std::nullopt;
    }
    Sha256Digest digest{};
    const NTSTATUS status = BCryptFinishHash(m_hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(m_hash);
    m_hash = nullptr;
    if (!nt_check(status, "BCryptFinishHash")) {
        return std::nullopt;
    }
    return digest;
}

std::optional<Sha256Digest> sha256(std::span<const std::uint8_t> data) {
    Sha256 hasher;
    if (!hasher.update(data)) {
        return std::nullopt;
    }
    return hasher.finish();
}

std::optional<Sha256Digest> sha256(std::string_view data) {
    return sha256(as_bytes(data));
}

std::optional<Sha256Digest> sha256_file(const std::filesystem::path& path, std::stop_token stop) {
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "sha256_file: CreateFileW");
        return std::nullopt;
    }
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{file};

    Sha256 hasher;
    std::vector<std::uint8_t> buffer(1024 * 1024);
    while (true) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "sha256_file: ReadFile");
            return std::nullopt;
        }
        if (read == 0) {
            break;
        }
        if (!hasher.update(std::span<const std::uint8_t>(buffer.data(), read))) {
            return std::nullopt;
        }
    }
    return hasher.finish();
}

// ---- Encoding -----------------------------------------------------------------------------------------

std::string to_hex(std::span<const std::uint8_t> data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t byte : data) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::optional<Bytes> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool hex_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int va = hex_value(a[i]);
        const int vb = hex_value(b[i]);
        if (va < 0 || vb < 0) {
            return false;
        }
        diff |= static_cast<unsigned>(va ^ vb);
    }
    return diff == 0;
}

std::string base64_encode(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return {};
    }
    DWORD size = 0;
    constexpr DWORD kFlags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), kFlags, nullptr, &size)) {
        return {};
    }
    std::string out(size, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), kFlags, out.data(), &size)) {
        return {};
    }
    out.resize(size);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

std::optional<Bytes> base64_decode(std::string_view text) {
    while (!text.empty() && std::strchr(" \t\r\n", text.front()) != nullptr) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::strchr(" \t\r\n", text.back()) != nullptr) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return Bytes{};
    }
    // CryptStringToBinaryA is lenient about stray characters; enforce the strict alphabet first.
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        const bool pad = ch == '=' && i >= text.size() - 2;
        if (!alpha && !pad) {
            return std::nullopt;
        }
        if (ch != '=' && i > 0 && text[i - 1] == '=') {
            return std::nullopt;
        }
    }
    DWORD size = 0;
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
        return std::nullopt;
    }
    Bytes out(size);
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, out.data(), &size, nullptr, nullptr)) {
        return std::nullopt;
    }
    out.resize(size);
    return out;
}

// ---- ECDSA P-256 --------------------------------------------------------------------------------------

std::optional<EcdsaP256KeyPair> ecdsa_p256_generate() {
    const UniqueAlg alg = open_ecdsa();
    if (!alg) {
        return std::nullopt;
    }
    BCRYPT_KEY_HANDLE raw_key = nullptr;
    if (!nt_check(BCryptGenerateKeyPair(alg.get(), &raw_key, 256, 0), "BCryptGenerateKeyPair")) {
        return std::nullopt;
    }
    const UniqueKey key(raw_key);
    if (!nt_check(BCryptFinalizeKeyPair(key.get(), 0), "BCryptFinalizeKeyPair")) {
        return std::nullopt;
    }
    auto pub = export_key(key.get(), false);
    auto priv = export_key(key.get(), true);
    if (!pub || !priv) {
        return std::nullopt;
    }
    return EcdsaP256KeyPair{.public_key = std::move(*pub), .private_key = std::move(*priv)};
}

std::optional<Bytes> ecdsa_p256_sign(std::span<const std::uint8_t> private_key, std::span<const std::uint8_t> message) {
    const auto digest = sha256(message);
    if (!digest) {
        return std::nullopt;
    }
    const UniqueAlg alg = open_ecdsa();
    if (!alg) {
        return std::nullopt;
    }
    const UniqueKey key = import_key(alg.get(), private_key, true);
    if (!key) {
        return std::nullopt;
    }
    Bytes signature(kEcdsaP256SignatureSize);
    ULONG size = 0;
    if (!nt_check(
            BCryptSignHash(
                key.get(),
                nullptr,
                const_cast<PUCHAR>(digest->data()),
                static_cast<ULONG>(digest->size()),
                signature.data(),
                static_cast<ULONG>(signature.size()),
                &size,
                0),
            "BCryptSignHash")) {
        return std::nullopt;
    }
    signature.resize(size);
    return signature;
}

bool ecdsa_p256_verify_digest(
    std::span<const std::uint8_t> public_key,
    const Sha256Digest& digest,
    std::span<const std::uint8_t> signature) {
    if (signature.size() != kEcdsaP256SignatureSize || public_key.size() != kEcdsaP256PublicKeySize) {
        return false;
    }
    const UniqueAlg alg = open_ecdsa();
    if (!alg) {
        return false;
    }
    const UniqueKey key = import_key(alg.get(), public_key, false);
    if (!key) {
        return false;
    }
    // STATUS_INVALID_SIGNATURE is the expected answer for a forged file: do not log it as an HRESULT failure.
    const NTSTATUS status = BCryptVerifySignature(
        key.get(),
        nullptr,
        const_cast<PUCHAR>(digest.data()),
        static_cast<ULONG>(digest.size()),
        const_cast<PUCHAR>(signature.data()),
        static_cast<ULONG>(signature.size()),
        0);
    return status == kStatusSuccess;
}

bool ecdsa_p256_verify(
    std::span<const std::uint8_t> public_key,
    std::span<const std::uint8_t> message,
    std::span<const std::uint8_t> signature) {
    const auto digest = sha256(message);
    if (!digest) {
        return false;
    }
    return ecdsa_p256_verify_digest(public_key, *digest, signature);
}

std::optional<Bytes> random_bytes(std::size_t count) {
    Bytes out(count);
    if (count == 0) {
        return out;
    }
    if (!nt_check(
            BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
            "BCryptGenRandom")) {
        return std::nullopt;
    }
    return out;
}

} // namespace boson::shipping
