#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::security {

/// @brief Failure of a security / storage operation. code is a Win32 error code (GetLastError) or one of
/// the ERROR_* values the APIs document; message is UTF-8 and safe to show to the frontend.
struct SecurityError {
    std::uint32_t code = 0;
    std::string message;
};

template <typename T>
using SecurityResult = std::expected<T, SecurityError>;

/// @brief SecurityError for code with "context: <FormatMessage text>" as message.
[[nodiscard]] SecurityError make_win32_error(std::uint32_t code, std::string_view context);
/// @brief make_win32_error(GetLastError(), context).
[[nodiscard]] SecurityError last_win32_error(std::string_view context);

/// @brief Standard (RFC 4648, padded) base64.
[[nodiscard]] std::string base64_encode(std::span<const std::byte> bytes);
/// @brief Decodes standard base64 (padding optional, no whitespace). nullopt-like error on invalid input.
[[nodiscard]] SecurityResult<std::vector<std::byte>> base64_decode(std::string_view text);

/// @brief Views text as bytes (no copy).
[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

/// @brief true when text is well-formed UTF-8 (required before handing file contents to glaze as a string).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// @brief Overwrites memory with zeros in a way the optimiser cannot elide (SecureZeroMemory).
void secure_zero(void* data, std::size_t size) noexcept;

/// @brief ASCII case-insensitive equality.
[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept;

} // namespace boson::security
