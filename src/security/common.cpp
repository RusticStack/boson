#include <boson/security/common.hpp>

#include <boson/core/result.hpp>

#include <windows.h>

#include <array>
#include <climits>

namespace boson::security {

namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::int8_t, 256> make_decode_table() {
    std::array<std::int8_t, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

constexpr std::array<std::int8_t, 256> kDecodeTable = make_decode_table();

char ascii_lower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

std::wstring_view format_system_message(DWORD code, DWORD lang, wchar_t*& buffer) noexcept {
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        lang,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr) {
        return {};
    }
    return std::wstring_view(buffer, length);
}

} // namespace

SecurityError make_win32_error(std::uint32_t code, std::string_view context) {
    SecurityError error{.code = code, .message = std::string(context)};
    wchar_t* buffer = nullptr;
    std::wstring_view text = format_system_message(code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buffer);
    if (text.empty()) {
        if (buffer != nullptr) {
            LocalFree(buffer);
            buffer = nullptr;
        }
        text = format_system_message(code, 0, buffer); // English message table missing: user language
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
        text.remove_suffix(1);
    }
    error.message += ": ";
    if (!text.empty()) {
        error.message += core::wide_to_utf8(text);
    } else {
        error.message += "Win32 error " + std::to_string(code);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return error;
}

SecurityError last_win32_error(std::string_view context) {
    return make_win32_error(GetLastError(), context);
}

std::string base64_encode(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (std::to_integer<std::uint32_t>(bytes[i]) << 16) |
                                     (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8) |
                                     std::to_integer<std::uint32_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t v = std::to_integer<std::uint32_t>(bytes[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.append("==");
    } else if (rest == 2) {
        const std::uint32_t v = (std::to_integer<std::uint32_t>(bytes[i]) << 16) |
                                (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

SecurityResult<std::vector<std::byte>> base64_decode(std::string_view text) {
    std::size_t padding = 0;
    while (!text.empty() && text.back() == '=' && padding < 2) {
        text.remove_suffix(1);
        ++padding;
    }
    if (text.size() % 4 == 1 || (padding != 0 && (text.size() + padding) % 4 != 0)) {
        return std::unexpected(SecurityError{.code = ERROR_INVALID_DATA, .message = "invalid base64: bad length"});
    }
    std::vector<std::byte> out;
    out.reserve((text.size() * 3) / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char ch : text) {
        const std::int8_t value = kDecodeTable[static_cast<unsigned char>(ch)];
        if (value < 0) {
            return std::unexpected(SecurityError{.code = ERROR_INVALID_DATA, .message = "invalid base64: unexpected character"});
        }
        acc = ((acc << 6) | static_cast<std::uint32_t>(value)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

bool is_valid_utf8(std::string_view text) noexcept {
    if (text.empty()) {
        return true;
    }
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

void secure_zero(void* data, std::size_t size) noexcept {
    if (data != nullptr && size != 0) {
        SecureZeroMemory(data, size);
    }
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace boson::security
