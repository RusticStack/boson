#include <boson/transfer/base64.hpp>

namespace boson::transfer {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::uint8_t kInvalid = 0xFF;

constexpr std::array<std::uint8_t, 256> make_decode_table() noexcept {
    std::array<std::uint8_t, 256> table{};
    for (auto& v : table) {
        v = kInvalid;
    }
    for (std::uint8_t i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = i;
    }
    return table;
}

constexpr std::array<std::uint8_t, 256> kDecode = make_decode_table();

} // namespace

void base64_encode_append(std::span<const std::byte> data, std::string& out) {
    const std::size_t start = out.size();
    out.resize(start + base64_encoded_size(data.size()));
    char* dst = out.data() + start;
    const auto* src = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t full = data.size() / 3 * 3;
    std::size_t i = 0;
    for (; i < full; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(src[i]) << 16) |
                                (static_cast<std::uint32_t>(src[i + 1]) << 8) |
                                static_cast<std::uint32_t>(src[i + 2]);
        dst[0] = kAlphabet[(v >> 18) & 0x3F];
        dst[1] = kAlphabet[(v >> 12) & 0x3F];
        dst[2] = kAlphabet[(v >> 6) & 0x3F];
        dst[3] = kAlphabet[v & 0x3F];
        dst += 4;
    }
    const std::size_t rest = data.size() - full;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(src[i]) << 16;
        dst[0] = kAlphabet[(v >> 18) & 0x3F];
        dst[1] = kAlphabet[(v >> 12) & 0x3F];
        dst[2] = '=';
        dst[3] = '=';
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(src[i]) << 16) | (static_cast<std::uint32_t>(src[i + 1]) << 8);
        dst[0] = kAlphabet[(v >> 18) & 0x3F];
        dst[1] = kAlphabet[(v >> 12) & 0x3F];
        dst[2] = kAlphabet[(v >> 6) & 0x3F];
        dst[3] = '=';
    }
}

bool base64_decode(std::string_view text, std::vector<std::byte>& out) {
    std::size_t padding = 0;
    while (!text.empty() && text.back() == '=' && padding < 2) {
        text.remove_suffix(1);
        ++padding;
    }
    if (text.size() % 4 == 1) {
        out.clear();
        return false;
    }
    if (padding != 0 && (text.size() + padding) % 4 != 0) {
        out.clear();
        return false;
    }
    out.resize(text.size() / 4 * 3 + (text.size() % 4 == 0 ? 0 : text.size() % 4 - 1));
    auto* dst = reinterpret_cast<std::uint8_t*>(out.data());
    const auto* src = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t full = text.size() / 4 * 4;
    std::size_t i = 0;
    for (; i < full; i += 4) {
        const std::uint8_t a = kDecode[src[i]];
        const std::uint8_t b = kDecode[src[i + 1]];
        const std::uint8_t c = kDecode[src[i + 2]];
        const std::uint8_t d = kDecode[src[i + 3]];
        if ((a | b | c | d) == kInvalid || a == kInvalid || b == kInvalid || c == kInvalid || d == kInvalid) {
            out.clear();
            return false;
        }
        const std::uint32_t v = (static_cast<std::uint32_t>(a) << 18) | (static_cast<std::uint32_t>(b) << 12) |
                                (static_cast<std::uint32_t>(c) << 6) | d;
        dst[0] = static_cast<std::uint8_t>(v >> 16);
        dst[1] = static_cast<std::uint8_t>(v >> 8);
        dst[2] = static_cast<std::uint8_t>(v);
        dst += 3;
    }
    const std::size_t rest = text.size() - full;
    if (rest >= 2) {
        const std::uint8_t a = kDecode[src[i]];
        const std::uint8_t b = kDecode[src[i + 1]];
        const std::uint8_t c = rest == 3 ? kDecode[src[i + 2]] : 0;
        if (a == kInvalid || b == kInvalid || c == kInvalid) {
            out.clear();
            return false;
        }
        const std::uint32_t v = (static_cast<std::uint32_t>(a) << 18) | (static_cast<std::uint32_t>(b) << 12) |
                                (static_cast<std::uint32_t>(c) << 6);
        dst[0] = static_cast<std::uint8_t>(v >> 16);
        if (rest == 3) {
            dst[1] = static_cast<std::uint8_t>(v >> 8);
        }
    }
    return true;
}

} // namespace boson::transfer
