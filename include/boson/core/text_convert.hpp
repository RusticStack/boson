#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace boson::core {

/// @brief UTF-8 -> UTF-16 into out, reusing its capacity. SSE2 fast path for all-ASCII input (JSON is almost always
/// ASCII), MultiByteToWideChar for the rest. Invalid input yields U+FFFD like MultiByteToWideChar; out is empty
/// on conversion failure.
void utf8_to_wide_into(std::string_view utf8, std::wstring& out);

/// @brief UTF-16 -> UTF-8 into out, reusing its capacity (same fast path / fallback split as utf8_to_wide_into).
void wide_to_utf8_into(std::wstring_view wide, std::string& out);

/// @brief UTF-8 form of a UTF-16 string that lives on the stack when it is short (no heap allocation for the
/// URLs and small JSON messages that dominate IPC traffic), on the heap otherwise. Not copyable: views stay valid
/// only while the object lives.
class Utf8Text {
public:
    explicit Utf8Text(std::wstring_view wide);
    Utf8Text(const Utf8Text&) = delete;
    Utf8Text& operator=(const Utf8Text&) = delete;
    Utf8Text(Utf8Text&&) = delete;
    Utf8Text& operator=(Utf8Text&&) = delete;
    ~Utf8Text();

    [[nodiscard]] std::string_view view() const noexcept { return m_view; }
    [[nodiscard]] bool empty() const noexcept { return m_view.empty(); }

private:
    static constexpr std::size_t kInlineBytes = 768;
    char m_inline[kInlineBytes];
    std::string m_heap;
    std::string_view m_view;
};

} // namespace boson::core
