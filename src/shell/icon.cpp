#include <boson/shell/icon.hpp>

#include <boson/core/result.hpp>

#include <shlobj_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace boson::shell {

namespace {

struct StockName {
    std::wstring_view name;
    SHSTOCKICONID id;
};

constexpr std::array kStockNames{
    StockName{L"info", SIID_INFO},
    StockName{L"warning", SIID_WARNING},
    StockName{L"error", SIID_ERROR},
    StockName{L"shield", SIID_SHIELD},
    StockName{L"help", SIID_HELP},
    StockName{L"delete", SIID_DELETE},
    StockName{L"find", SIID_FIND},
    StockName{L"settings", SIID_SETTINGS},
    StockName{L"folder", SIID_FOLDER},
    StockName{L"document", SIID_DOCNOASSOC},
    StockName{L"lock", SIID_LOCK},
    StockName{L"key", SIID_KEY},
    StockName{L"world", SIID_WORLD},
    StockName{L"users", SIID_USERS},
    StockName{L"link", SIID_LINK},
    StockName{L"printer", SIID_PRINTER},
    StockName{L"rename", SIID_RENAME},
    StockName{L"recycler", SIID_RECYCLER},
    StockName{L"share", SIID_SHARE},
    StockName{L"software", SIID_SOFTWARE},
};

class MemoryDc {
public:
    MemoryDc() noexcept : m_dc(::CreateCompatibleDC(nullptr)) {}
    ~MemoryDc() {
        if (m_dc != nullptr) {
            ::DeleteDC(m_dc);
        }
    }
    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;
    [[nodiscard]] HDC get() const noexcept { return m_dc; }

private:
    HDC m_dc;
};

class GdiObject {
public:
    explicit GdiObject(HGDIOBJ obj = nullptr) noexcept : m_obj(obj) {}
    ~GdiObject() {
        if (m_obj != nullptr) {
            ::DeleteObject(m_obj);
        }
    }
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
    [[nodiscard]] HGDIOBJ get() const noexcept { return m_obj; }

private:
    HGDIOBJ m_obj;
};

HBITMAP create_dib(int size, std::uint32_t*& bits) noexcept {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bmp = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    bits = static_cast<std::uint32_t*>(raw);
    return bmp;
}

/// Renders text as a grayscale coverage mask (0..255 per pixel). Empty when GDI fails.
std::vector<std::uint8_t> text_coverage(
    int size, std::wstring_view text, const wchar_t* face, int font_px, int weight, bool require_face) {
    std::vector<std::uint8_t> coverage;
    if (text.empty()) {
        coverage.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0);
        return coverage;
    }
    const MemoryDc dc;
    if (dc.get() == nullptr) {
        return coverage;
    }
    std::uint32_t* bits = nullptr;
    const GdiObject bmp(create_dib(size, bits));
    if (bmp.get() == nullptr || bits == nullptr) {
        return coverage;
    }
    const GdiObject font(::CreateFontW(-font_px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                       DEFAULT_PITCH | FF_DONTCARE, face));
    if (font.get() == nullptr) {
        return coverage;
    }
    const HGDIOBJ old_bmp = ::SelectObject(dc.get(), bmp.get());
    const HGDIOBJ old_font = ::SelectObject(dc.get(), font.get());
    bool face_ok = true;
    if (require_face) {
        wchar_t actual[LF_FACESIZE]{};
        ::GetTextFaceW(dc.get(), LF_FACESIZE, actual);
        face_ok = ::_wcsicmp(actual, face) == 0;
    }
    if (face_ok) {
        std::fill_n(bits, static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0u);
        ::SetBkMode(dc.get(), TRANSPARENT);
        ::SetTextColor(dc.get(), RGB(255, 255, 255));
        RECT rc{0, 0, size, size};
        ::DrawTextW(dc.get(), text.data(), static_cast<int>(text.size()), &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        ::GdiFlush();
        coverage.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            const std::uint32_t px = bits[i];
            const std::uint8_t r = static_cast<std::uint8_t>((px >> 16) & 0xFF);
            const std::uint8_t g = static_cast<std::uint8_t>((px >> 8) & 0xFF);
            const std::uint8_t b = static_cast<std::uint8_t>(px & 0xFF);
            coverage[i] = (std::max)({r, g, b});
        }
    }
    ::SelectObject(dc.get(), old_font);
    ::SelectObject(dc.get(), old_bmp);
    return coverage;
}

/// Composites foreground coverage over an optional anti-aliased disc into a straight-alpha icon.
Icon compose_icon(int size, const std::vector<std::uint8_t>& coverage, COLORREF fg, std::optional<COLORREF> disc) {
    if (coverage.size() != static_cast<std::size_t>(size) * static_cast<std::size_t>(size)) {
        return {};
    }
    std::uint32_t* bits = nullptr;
    const GdiObject color(create_dib(size, bits));
    const GdiObject mask(::CreateBitmap(size, size, 1, 1, nullptr));
    if (color.get() == nullptr || bits == nullptr || mask.get() == nullptr) {
        return {};
    }
    const double radius = size / 2.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
            const double t = coverage[i] / 255.0;
            double c = 0.0;
            if (disc.has_value()) {
                const double dx = x + 0.5 - radius;
                const double dy = y + 0.5 - radius;
                c = std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0, 1.0);
            }
            const double alpha = t + c * (1.0 - t);
            if (alpha <= 0.0) {
                bits[i] = 0;
                continue;
            }
            const auto channel = [&](BYTE f, BYTE b) {
                const double v = (f * t + b * c * (1.0 - t)) / alpha;
                return static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0) + 0.5);
            };
            const COLORREF bg = disc.value_or(RGB(0, 0, 0));
            const std::uint32_t r = channel(GetRValue(fg), GetRValue(bg));
            const std::uint32_t g = channel(GetGValue(fg), GetGValue(bg));
            const std::uint32_t b = channel(GetBValue(fg), GetBValue(bg));
            const std::uint32_t a = static_cast<std::uint32_t>(alpha * 255.0 + 0.5);
            bits[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = static_cast<HBITMAP>(color.get());
    info.hbmMask = static_cast<HBITMAP>(mask.get());
    return Icon(::CreateIconIndirect(&info));
}

} // namespace

int default_small_icon_size() noexcept {
    const int size = ::GetSystemMetrics(SM_CXSMICON);
    return size > 0 ? size : 16;
}

Icon Icon::from_file(std::wstring_view path, int size_px) {
    if (path.empty()) {
        return {};
    }
    std::wstring file(path);
    int index = 0;
    const std::size_t comma = file.find_last_of(L',');
    if (comma != std::wstring::npos && comma + 1 < file.size()) {
        const std::wstring tail = file.substr(comma + 1);
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(tail.c_str(), &end, 10);
        if (end != nullptr && *end == L'\0') {
            index = static_cast<int>(parsed);
            file.resize(comma);
        }
    }
    const int size = size_px > 0 ? size_px : default_small_icon_size();
    HICON large = nullptr;
    const HRESULT hr = ::SHDefExtractIconW(file.c_str(), index, 0, &large, nullptr,
                                           static_cast<UINT>(MAKELONG(size, size)));
    if (hr != S_OK || large == nullptr) {
        return {};
    }
    return Icon(large);
}

Icon Icon::from_image_bytes(std::span<const std::byte> bytes, int size_px) {
    if (bytes.empty() || bytes.size() > 0x7FFFFFFF) {
        return {};
    }
    const int size = size_px > 0 ? size_px : default_small_icon_size();
    // CreateIconFromResourceEx does not write to the buffer; the API just is not const-correct.
    auto* data = reinterpret_cast<PBYTE>(const_cast<std::byte*>(bytes.data()));
    return Icon(::CreateIconFromResourceEx(data, static_cast<DWORD>(bytes.size()), TRUE, 0x00030000,
                                           size, size, LR_DEFAULTCOLOR));
}

Icon Icon::stock(SHSTOCKICONID id, bool small_icon) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    const UINT flags = SHGSI_ICON | (small_icon ? SHGSI_SMALLICON : SHGSI_LARGEICON);
    if (!core::hr_check(::SHGetStockIconInfo(id, flags, &info), "SHGetStockIconInfo")) {
        return {};
    }
    return Icon(info.hIcon);
}

Icon Icon::stock(std::wstring_view name, bool small_icon) {
    for (const auto& entry : kStockNames) {
        if (entry.name == name) {
            return stock(entry.id, small_icon);
        }
    }
    return {};
}

Icon Icon::badge(std::wstring_view text, COLORREF background, COLORREF foreground, int size_px) {
    const int size = size_px > 0 ? size_px : default_small_icon_size();
    const double scale = text.size() <= 1 ? 0.72 : (text.size() == 2 ? 0.58 : 0.44);
    const int font_px = (std::max)(6, static_cast<int>(size * scale));
    const auto coverage = text_coverage(size, text, L"Segoe UI", font_px, FW_BOLD, false);
    return compose_icon(size, coverage, foreground, background);
}

Icon Icon::glyph(wchar_t codepoint, COLORREF color, int size_px) {
    const int size = size_px > 0 ? size_px : default_small_icon_size();
    const wchar_t text[1]{codepoint};
    const int font_px = (std::max)(6, static_cast<int>(size * 0.85));
    auto coverage = text_coverage(size, std::wstring_view(text, 1), L"Segoe Fluent Icons", font_px, FW_NORMAL, true);
    if (coverage.empty()) {
        coverage = text_coverage(size, std::wstring_view(text, 1), L"Segoe MDL2 Assets", font_px, FW_NORMAL, false);
    }
    return compose_icon(size, coverage, color, std::nullopt);
}

} // namespace boson::shell
