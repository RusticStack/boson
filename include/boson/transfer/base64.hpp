#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::transfer {

/// @brief RFC 4648 base64 (standard alphabet, padded). Used by the JSON fallback when shared buffers are
/// unavailable.
[[nodiscard]] constexpr std::size_t base64_encoded_size(std::size_t bytes) noexcept {
    return (bytes + 2) / 3 * 4;
}

/// @brief Appends the base64 encoding of data to out.
void base64_encode_append(std::span<const std::byte> data, std::string& out);

[[nodiscard]] inline std::string base64_encode(std::span<const std::byte> data) {
    std::string out;
    base64_encode_append(data, out);
    return out;
}

/// @brief Decodes padded or unpadded base64 into out (resized to the decoded length). Rejects any character
/// outside the alphabet, including whitespace. Returns false on malformed input.
bool base64_decode(std::string_view text, std::vector<std::byte>& out);

[[nodiscard]] inline std::optional<std::vector<std::byte>> base64_decode(std::string_view text) {
    std::vector<std::byte> out;
    if (!base64_decode(text, out)) {
        return std::nullopt;
    }
    return out;
}

} // namespace boson::transfer
