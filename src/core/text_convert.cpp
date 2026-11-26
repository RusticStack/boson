#include "boson/core/text_convert.hpp"

#include <windows.h>

#include <emmintrin.h>

#include <cstdint>

namespace boson::core {

namespace {

/// true when every UTF-16 code unit is < 0x80.
bool is_ascii(const wchar_t* p, std::size_t n) noexcept {
    std::size_t i = 0;
    const __m128i high = _mm_set1_epi16(static_cast<short>(0xFF80));
    __m128i acc = _mm_setzero_si128();
    for (; i + 8 <= n; i += 8) {
        acc = _mm_or_si128(acc, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i)));
        if ((i & 0x3F) == 0x38 && _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_and_si128(acc, high), _mm_setzero_si128())) != 0xFFFF) {
            return false;
        }
    }
    std::uint16_t tail = 0;
    for (; i < n; ++i) {
        tail = static_cast<std::uint16_t>(tail | static_cast<std::uint16_t>(p[i]));
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_and_si128(acc, high), _mm_setzero_si128())) == 0xFFFF && (tail & 0xFF80U) == 0;
}

/// true when every byte is < 0x80.
bool is_ascii(const char* p, std::size_t n) noexcept {
    std::size_t i = 0;
    __m128i acc = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16) {
        acc = _mm_or_si128(acc, _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i)));
        if ((i & 0x7F) == 0x70 && _mm_movemask_epi8(acc) != 0) {
            return false;
        }
    }
    unsigned char tail = 0;
    for (; i < n; ++i) {
        tail = static_cast<unsigned char>(tail | static_cast<unsigned char>(p[i]));
    }
    return _mm_movemask_epi8(acc) == 0 && (tail & 0x80U) == 0;
}

void narrow_ascii(const wchar_t* src, char* dst, std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 8));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_packus_epi16(a, b));
    }
    for (; i < n; ++i) {
        dst[i] = static_cast<char>(src[i]);
    }
}

void widen_ascii(const char* src, wchar_t* dst, std::size_t n) noexcept {
    std::size_t i = 0;
    const __m128i zero = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_unpacklo_epi8(v, zero));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 8), _mm_unpackhi_epi8(v, zero));
    }
    for (; i < n; ++i) {
        dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
    }
}

/// Exact UTF-8 length of non-ASCII UTF-16 text (0 on failure).
int utf8_length_api(const wchar_t* p, std::size_t n) noexcept {
    return WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
}

} // namespace

void utf8_to_wide_into(std::string_view utf8, std::wstring& out) {
    out.clear();
    if (utf8.empty() || utf8.size() > 0x7FFFFFFFU) {
        return;
    }
    const std::size_t n = utf8.size();
    if (is_ascii(utf8.data(), n)) {
        out.resize_and_overwrite(n, [&](wchar_t* dst, std::size_t) noexcept {
            widen_ascii(utf8.data(), dst, n);
            return n;
        });
        return;
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(n), nullptr, 0);
    if (required <= 0) {
        return;
    }
    out.resize_and_overwrite(static_cast<std::size_t>(required), [&](wchar_t* dst, std::size_t cap) noexcept {
        const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(n), dst, static_cast<int>(cap));
        return written > 0 ? static_cast<std::size_t>(written) : std::size_t{0};
    });
}

void wide_to_utf8_into(std::wstring_view wide, std::string& out) {
    out.clear();
    if (wide.empty() || wide.size() > 0x7FFFFFFFU) {
        return;
    }
    const std::size_t n = wide.size();
    if (is_ascii(wide.data(), n)) {
        out.resize_and_overwrite(n, [&](char* dst, std::size_t) noexcept {
            narrow_ascii(wide.data(), dst, n);
            return n;
        });
        return;
    }
    const int required = utf8_length_api(wide.data(), n);
    if (required <= 0) {
        return;
    }
    out.resize_and_overwrite(static_cast<std::size_t>(required), [&](char* dst, std::size_t cap) noexcept {
        const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(n), dst, static_cast<int>(cap), nullptr, nullptr);
        return written > 0 ? static_cast<std::size_t>(written) : std::size_t{0};
    });
}

namespace {
// One recycled heap buffer per thread for large messages: allocating (and page-faulting) a fresh 1 MiB string for
// every message dominated the conversion itself. Taken on construction, returned on destruction, so nested
// conversions (a handler that pumps messages) simply allocate their own.
constexpr std::size_t kMaxRecycledBytes = 1u << 20;
thread_local std::string t_recycled;
} // namespace

Utf8Text::Utf8Text(std::wstring_view wide) {
    const std::size_t n = wide.size();
    if (n == 0 || n > 0x7FFFFFFFU) {
        return;
    }
    if (n < kInlineBytes && is_ascii(wide.data(), n)) {
        narrow_ascii(wide.data(), m_inline, n);
        m_inline[n] = '\0'; // views handed to the router may be read as NUL-terminated
        m_view = std::string_view{m_inline, n};
        return;
    }
    if (n * 3 < kInlineBytes) {
        const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(n), m_inline, static_cast<int>(kInlineBytes - 1), nullptr, nullptr);
        if (written > 0) {
            m_inline[written] = '\0';
            m_view = std::string_view{m_inline, static_cast<std::size_t>(written)};
        }
        return;
    }
    m_heap = std::move(t_recycled);
    t_recycled = std::string{};
    wide_to_utf8_into(wide, m_heap);
    m_view = m_heap;
}

Utf8Text::~Utf8Text() {
    if (m_heap.capacity() > kMaxRecycledBytes) {
        return;
    }
    if (m_heap.capacity() > t_recycled.capacity()) {
        m_heap.clear();
        t_recycled = std::move(m_heap);
    }
}

} // namespace boson::core
