#include <boson/desktop/clipboard.hpp>

#include <boson/core/result.hpp>

#include "desktop_util.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <limits>
#include <thread>

namespace boson::desktop {

using Microsoft::WRL::ComPtr;

// ---- CF_HTML ---------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kHtmlPrefix = "<html><body>\r\n<!--StartFragment-->";
constexpr std::string_view kHtmlSuffix = "<!--EndFragment-->\r\n</body></html>";
constexpr std::string_view kStartFragmentMarker = "<!--StartFragment-->";
constexpr std::string_view kEndFragmentMarker = "<!--EndFragment-->";

void append_offset(std::string& out, std::string_view key, std::size_t value) {
    char digits[16]{};
    std::snprintf(digits, sizeof(digits), "%010zu", value);
    out += key;
    out += ':';
    out += digits;
    out += "\r\n";
}

std::string build_header(std::size_t start_html, std::size_t end_html, std::size_t start_fragment,
                         std::size_t end_fragment, std::string_view source_url) {
    std::string header = "Version:0.9\r\n";
    append_offset(header, "StartHTML", start_html);
    append_offset(header, "EndHTML", end_html);
    append_offset(header, "StartFragment", start_fragment);
    append_offset(header, "EndFragment", end_fragment);
    if (!source_url.empty()) {
        header += "SourceURL:";
        header += source_url;
        header += "\r\n";
    }
    return header;
}

std::optional<long long> header_number(std::string_view header, std::string_view key) {
    std::size_t pos = 0;
    while (pos < header.size()) {
        const std::size_t eol = header.find_first_of("\r\n", pos);
        const std::string_view line = header.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        if (line.size() > key.size() && line[key.size()] == ':' && detail::iequals(line.substr(0, key.size()), key)) {
            std::string_view value = line.substr(key.size() + 1);
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            long long number = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (ec != std::errc{} || ptr == value.data()) {
                return std::nullopt;
            }
            return number;
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = eol + 1;
    }
    return std::nullopt;
}

std::string header_string(std::string_view header, std::string_view key) {
    std::size_t pos = 0;
    while (pos < header.size()) {
        const std::size_t eol = header.find_first_of("\r\n", pos);
        const std::string_view line = header.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        if (line.size() > key.size() && line[key.size()] == ':' && detail::iequals(line.substr(0, key.size()), key)) {
            return std::string(line.substr(key.size() + 1));
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = eol + 1;
    }
    return {};
}

bool valid_range(long long begin, long long end, std::size_t size) noexcept {
    return begin >= 0 && end >= begin && static_cast<unsigned long long>(end) <= size;
}

} // namespace

std::string build_cf_html(std::string_view fragment_html, std::string_view source_url) {
    std::string url;
    for (const char ch : source_url) {
        if (ch != '\r' && ch != '\n') {
            url.push_back(ch);
        }
    }
    // Fixed-width offsets: the header length does not depend on the values written into it.
    const std::size_t header_size = build_header(0, 0, 0, 0, url).size();
    const std::size_t start_html = header_size;
    const std::size_t start_fragment = start_html + kHtmlPrefix.size();
    const std::size_t end_fragment = start_fragment + fragment_html.size();
    const std::size_t end_html = end_fragment + kHtmlSuffix.size();

    std::string out = build_header(start_html, end_html, start_fragment, end_fragment, url);
    out.reserve(end_html + 1);
    out += kHtmlPrefix;
    out += fragment_html;
    out += kHtmlSuffix;
    return out;
}

std::optional<CfHtml> parse_cf_html(std::string_view cf_html) {
    while (!cf_html.empty() && cf_html.back() == '\0') {
        cf_html.remove_suffix(1);
    }
    if (cf_html.empty()) {
        return std::nullopt;
    }
    const std::size_t first_tag = cf_html.find('<');
    const std::string_view header = cf_html.substr(0, first_tag == std::string_view::npos ? cf_html.size() : first_tag);
    if (!header_number(header, "Version") && header.find("Version:") == std::string_view::npos) {
        return std::nullopt;
    }

    const auto start_html = header_number(header, "StartHTML");
    const auto end_html = header_number(header, "EndHTML");
    const auto start_fragment = header_number(header, "StartFragment");
    const auto end_fragment = header_number(header, "EndFragment");

    CfHtml result;
    result.source_url = header_string(header, "SourceURL");

    if (start_fragment && end_fragment && valid_range(*start_fragment, *end_fragment, cf_html.size())) {
        result.fragment = std::string(cf_html.substr(
            static_cast<std::size_t>(*start_fragment),
            static_cast<std::size_t>(*end_fragment - *start_fragment)));
    } else {
        // Broken offsets (seen in the wild): fall back to the fragment comments.
        const std::size_t begin = cf_html.find(kStartFragmentMarker);
        const std::size_t end = cf_html.find(kEndFragmentMarker);
        if (begin == std::string_view::npos || end == std::string_view::npos || end < begin) {
            return std::nullopt;
        }
        const std::size_t content = begin + kStartFragmentMarker.size();
        result.fragment = std::string(cf_html.substr(content, end - content));
    }

    if (start_html && end_html && valid_range(*start_html, *end_html, cf_html.size())) {
        result.html = std::string(cf_html.substr(
            static_cast<std::size_t>(*start_html),
            static_cast<std::size_t>(*end_html - *start_html)));
    } else if (first_tag != std::string_view::npos) {
        result.html = std::string(cf_html.substr(first_tag));
    } else {
        result.html = result.fragment;
    }
    return result;
}

// ---- Base64 ----------------------------------------------------------------------------------------------

std::string base64_encode(std::span<const std::byte> bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v = (std::to_integer<std::uint32_t>(bytes[i]) << 16) |
                                (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8) |
                                std::to_integer<std::uint32_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t v = std::to_integer<std::uint32_t>(bytes[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out += "==";
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

std::optional<std::vector<std::byte>> base64_decode(std::string_view text) {
    const auto value_of = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+' || ch == '-') return 62;
        if (ch == '/' || ch == '_') return 63;
        return -1;
    };
    // Accept data: URLs ("data:image/png;base64,....").
    if (text.starts_with("data:")) {
        const std::size_t comma = text.find(',');
        if (comma == std::string_view::npos) {
            return std::nullopt;
        }
        text.remove_prefix(comma + 1);
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (const char ch : text) {
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') {
            continue;
        }
        if (ch == '=') {
            padding = true;
            continue;
        }
        if (padding) {
            return std::nullopt; // data after padding
        }
        const int v = value_of(ch);
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFF));
        }
    }
    if (bits >= 6) {
        return std::nullopt; // a single dangling sextet cannot encode a byte
    }
    return out;
}

// ---- WIC image conversion --------------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kMaxImagePixels = 1ull << 28; // 268M pixels (1 GiB of BGRA) hard cap

ComPtr<IWICImagingFactory> wic_factory() {
    ComPtr<IWICImagingFactory> factory;
    if (!core::hr_check(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "CoCreateInstance(CLSID_WICImagingFactory)")) {
        return nullptr;
    }
    return factory;
}

ComPtr<IStream> memory_stream(std::span<const std::byte> bytes) {
    if (bytes.size() > std::numeric_limits<UINT>::max()) {
        return nullptr;
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    return stream;
}

std::optional<BgraImage> decode_with(std::span<const std::byte> bytes, const GUID& container) {
    const detail::ScopedCom com;
    if (!com.ok()) {
        return std::nullopt;
    }
    const auto factory = wic_factory();
    const auto stream = memory_stream(bytes);
    if (!factory || !stream) {
        return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (!BOSON_HR_CHECK(factory->CreateDecoder(container, nullptr, &decoder)) ||
        FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnLoad))) {
        return std::nullopt;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return std::nullopt;
    }
    ComPtr<IWICFormatConverter> converter;
    if (!BOSON_HR_CHECK(factory->CreateFormatConverter(&converter)) ||
        !BOSON_HR_CHECK(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom))) {
        return std::nullopt;
    }
    UINT width = 0;
    UINT height = 0;
    if (!BOSON_HR_CHECK(converter->GetSize(&width, &height)) || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > kMaxImagePixels) {
        return std::nullopt;
    }
    BgraImage image;
    image.width = width;
    image.height = height;
    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4;
    image.pixels.resize(static_cast<std::size_t>(stride * height));
    if (!BOSON_HR_CHECK(converter->CopyPixels(
            nullptr, static_cast<UINT>(stride), static_cast<UINT>(image.pixels.size()),
            reinterpret_cast<BYTE*>(image.pixels.data())))) {
        return std::nullopt;
    }
    return image;
}

template <typename T>
T read_le(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::uint8_t extract_channel(std::uint32_t pixel, std::uint32_t mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    const int shift = std::countr_zero(mask);
    const std::uint32_t bits = static_cast<std::uint32_t>(std::popcount(mask >> shift));
    const std::uint32_t value = (pixel & mask) >> shift;
    if (bits == 8) {
        return static_cast<std::uint8_t>(value);
    }
    const std::uint32_t max = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) * 255u + max / 2) / max);
}

/// 32bpp BI_RGB / BI_BITFIELDS decoded by hand: WIC's BMP codec ignores the alpha of a BI_RGB DIB.
std::optional<BgraImage> decode_dib32(std::span<const std::byte> dib) {
    const auto header_size = read_le<std::uint32_t>(dib, 0);
    const auto width = read_le<std::int32_t>(dib, 4);
    const auto raw_height = read_le<std::int32_t>(dib, 8);
    const auto compression = read_le<std::uint32_t>(dib, 16);
    const auto colors_used = read_le<std::uint32_t>(dib, 32);
    if (width <= 0 || raw_height == 0 || raw_height == std::numeric_limits<std::int32_t>::min()) {
        return std::nullopt;
    }
    const bool top_down = raw_height < 0;
    const std::uint32_t height = static_cast<std::uint32_t>(top_down ? -raw_height : raw_height);
    const std::uint32_t w = static_cast<std::uint32_t>(width);
    if (static_cast<std::uint64_t>(w) * height > kMaxImagePixels) {
        return std::nullopt;
    }

    std::uint32_t red = 0x00FF0000u;
    std::uint32_t green = 0x0000FF00u;
    std::uint32_t blue = 0x000000FFu;
    std::uint32_t alpha = 0;
    std::size_t offset = header_size;
    const bool bitfields = compression == BI_BITFIELDS || compression == 6 /* BI_ALPHABITFIELDS */;
    if (bitfields) {
        if (header_size >= 52) {
            red = read_le<std::uint32_t>(dib, 40);
            green = read_le<std::uint32_t>(dib, 44);
            blue = read_le<std::uint32_t>(dib, 48);
            if (header_size >= 56) {
                alpha = read_le<std::uint32_t>(dib, 52);
            }
        } else {
            const std::size_t mask_bytes = compression == 6 ? 16 : 12;
            if (dib.size() < header_size + mask_bytes) {
                return std::nullopt;
            }
            red = read_le<std::uint32_t>(dib, header_size);
            green = read_le<std::uint32_t>(dib, header_size + 4);
            blue = read_le<std::uint32_t>(dib, header_size + 8);
            if (compression == 6) {
                alpha = read_le<std::uint32_t>(dib, header_size + 12);
            }
            offset += mask_bytes;
        }
    } else if (header_size >= 56) {
        alpha = read_le<std::uint32_t>(dib, 52);
    }
    offset += static_cast<std::size_t>(colors_used) * 4;

    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    if (dib.size() < offset || dib.size() - offset < stride * height) {
        return std::nullopt;
    }

    // BI_RGB 32bpp: the high byte is formally reserved; browsers put alpha there. Use it unless all zero.
    bool alpha_from_reserved = false;
    if (compression == BI_RGB && alpha == 0) {
        for (std::size_t i = 0; i < stride * height; i += 4) {
            if (std::to_integer<std::uint8_t>(dib[offset + i + 3]) != 0) {
                alpha_from_reserved = true;
                break;
            }
        }
        if (alpha_from_reserved) {
            alpha = 0xFF000000u;
        }
    }

    BgraImage image;
    image.width = w;
    image.height = height;
    image.pixels.resize(stride * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t src_row = offset + stride * (top_down ? y : (height - 1 - y));
        std::byte* dst = image.pixels.data() + stride * y;
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto pixel = read_le<std::uint32_t>(dib, src_row + static_cast<std::size_t>(x) * 4);
            dst[x * 4 + 0] = static_cast<std::byte>(extract_channel(pixel, blue));
            dst[x * 4 + 1] = static_cast<std::byte>(extract_channel(pixel, green));
            dst[x * 4 + 2] = static_cast<std::byte>(extract_channel(pixel, red));
            dst[x * 4 + 3] = static_cast<std::byte>(alpha != 0 ? extract_channel(pixel, alpha) : 0xFF);
        }
    }
    return image;
}

} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> png_dimensions(std::span<const std::byte> png) noexcept {
    static constexpr std::array<unsigned char, 8> kSignature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 24) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kSignature.size(); ++i) {
        if (std::to_integer<unsigned char>(png[i]) != kSignature[i]) {
            return std::nullopt;
        }
    }
    const auto be32 = [&png](std::size_t at) {
        return (std::to_integer<std::uint32_t>(png[at]) << 24) | (std::to_integer<std::uint32_t>(png[at + 1]) << 16) |
               (std::to_integer<std::uint32_t>(png[at + 2]) << 8) | std::to_integer<std::uint32_t>(png[at + 3]);
    };
    if (std::to_integer<char>(png[12]) != 'I' || std::to_integer<char>(png[13]) != 'H' ||
        std::to_integer<char>(png[14]) != 'D' || std::to_integer<char>(png[15]) != 'R') {
        return std::nullopt;
    }
    return std::pair{be32(16), be32(20)};
}

std::optional<BgraImage> decode_png(std::span<const std::byte> png) {
    if (!png_dimensions(png)) {
        return std::nullopt;
    }
    return decode_with(png, GUID_ContainerFormatPng);
}

std::optional<std::vector<std::byte>> encode_png(const BgraImage& image) {
    const std::uint64_t stride = static_cast<std::uint64_t>(image.width) * 4;
    if (image.width == 0 || image.height == 0 ||
        static_cast<std::uint64_t>(image.width) * image.height > kMaxImagePixels ||
        image.pixels.size() != stride * image.height) {
        return std::nullopt;
    }
    const detail::ScopedCom com;
    if (!com.ok()) {
        return std::nullopt;
    }
    const auto factory = wic_factory();
    if (!factory) {
        return std::nullopt;
    }
    ComPtr<IStream> stream;
    if (!BOSON_HR_CHECK(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return std::nullopt;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (!BOSON_HR_CHECK(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        !BOSON_HR_CHECK(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        !BOSON_HR_CHECK(encoder->CreateNewFrame(&frame, &props)) ||
        !BOSON_HR_CHECK(frame->Initialize(props.Get())) ||
        !BOSON_HR_CHECK(frame->SetSize(image.width, image.height))) {
        return std::nullopt;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!BOSON_HR_CHECK(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) {
        return std::nullopt;
    }
    // WritePixels only reads the buffer; the parameter is non-const for historical reasons.
    auto* pixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data()));
    if (!BOSON_HR_CHECK(frame->WritePixels(image.height, static_cast<UINT>(stride), static_cast<UINT>(image.pixels.size()), pixels)) ||
        !BOSON_HR_CHECK(frame->Commit()) || !BOSON_HR_CHECK(encoder->Commit())) {
        return std::nullopt;
    }
    STATSTG stat{};
    if (!BOSON_HR_CHECK(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > std::numeric_limits<ULONG>::max()) {
        return std::nullopt;
    }
    std::vector<std::byte> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER zero{};
    ULONG read = 0;
    if (!BOSON_HR_CHECK(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        !BOSON_HR_CHECK(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) || read != out.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::byte>> png_to_dibv5(std::span<const std::byte> png) {
    const auto image = decode_png(png);
    if (!image) {
        return std::nullopt;
    }
    const std::size_t stride = static_cast<std::size_t>(image->width) * 4;
    const std::size_t pixel_bytes = stride * image->height;

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = static_cast<LONG>(image->width);
    header.bV5Height = static_cast<LONG>(image->height); // positive: bottom-up, the most compatible layout
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(pixel_bytes);
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;

    std::vector<std::byte> dib(sizeof(BITMAPV5HEADER) + pixel_bytes);
    std::memcpy(dib.data(), &header, sizeof(header));
    for (std::uint32_t y = 0; y < image->height; ++y) {
        std::memcpy(
            dib.data() + sizeof(BITMAPV5HEADER) + stride * (image->height - 1 - y),
            image->pixels.data() + stride * y,
            stride);
    }
    return dib;
}

std::optional<std::vector<std::byte>> dib_to_png(std::span<const std::byte> dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) {
        return std::nullopt;
    }
    const auto header_size = read_le<std::uint32_t>(dib, 0);
    if (header_size < sizeof(BITMAPINFOHEADER) || header_size > dib.size()) {
        return std::nullopt;
    }
    const auto bit_count = read_le<std::uint16_t>(dib, 14);
    const auto compression = read_le<std::uint32_t>(dib, 16);
    if (bit_count == 32 && (compression == BI_RGB || compression == BI_BITFIELDS || compression == 6)) {
        const auto image = decode_dib32(dib);
        return image ? encode_png(*image) : std::nullopt;
    }

    // Everything else (palettes, 16/24 bpp, RLE): wrap as a .bmp file and let WIC decode it.
    const auto colors_used = read_le<std::uint32_t>(dib, 32);
    std::size_t palette_entries = colors_used;
    if (palette_entries == 0 && bit_count <= 8) {
        palette_entries = std::size_t{1} << bit_count;
    }
    std::size_t offset = header_size + palette_entries * 4;
    if (compression == BI_BITFIELDS && header_size == sizeof(BITMAPINFOHEADER)) {
        offset += 12;
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42; // "BM"
    file.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + offset);
    std::vector<std::byte> bmp(sizeof(BITMAPFILEHEADER) + dib.size());
    std::memcpy(bmp.data(), &file, sizeof(file));
    std::memcpy(bmp.data() + sizeof(BITMAPFILEHEADER), dib.data(), dib.size());
    const auto image = decode_with(bmp, GUID_ContainerFormatBmp);
    return image ? encode_png(*image) : std::nullopt;
}

// ---- Clipboard -------------------------------------------------------------------------------------------

namespace {

struct StandardFormat {
    UINT id;
    std::string_view name;
};

constexpr std::array<StandardFormat, 22> kStandardFormats{{
    {CF_TEXT, "CF_TEXT"},
    {CF_BITMAP, "CF_BITMAP"},
    {CF_METAFILEPICT, "CF_METAFILEPICT"},
    {CF_SYLK, "CF_SYLK"},
    {CF_DIF, "CF_DIF"},
    {CF_TIFF, "CF_TIFF"},
    {CF_OEMTEXT, "CF_OEMTEXT"},
    {CF_DIB, "CF_DIB"},
    {CF_PALETTE, "CF_PALETTE"},
    {CF_PENDATA, "CF_PENDATA"},
    {CF_RIFF, "CF_RIFF"},
    {CF_WAVE, "CF_WAVE"},
    {CF_UNICODETEXT, "CF_UNICODETEXT"},
    {CF_ENHMETAFILE, "CF_ENHMETAFILE"},
    {CF_HDROP, "CF_HDROP"},
    {CF_LOCALE, "CF_LOCALE"},
    {CF_DIBV5, "CF_DIBV5"},
    {CF_OWNERDISPLAY, "CF_OWNERDISPLAY"},
    {CF_DSPTEXT, "CF_DSPTEXT"},
    {CF_DSPBITMAP, "CF_DSPBITMAP"},
    {CF_DSPMETAFILEPICT, "CF_DSPMETAFILEPICT"},
    {CF_DSPENHMETAFILE, "CF_DSPENHMETAFILE"},
}};

UINT registered(const wchar_t* name) noexcept {
    return RegisterClipboardFormatW(name);
}

UINT html_format() noexcept {
    static const UINT id = registered(L"HTML Format");
    return id;
}

UINT png_format() noexcept {
    static const UINT id = registered(L"PNG");
    return id;
}

/// RAII OpenClipboard with a retry window (another process may hold the clipboard briefly).
class ClipboardSession {
public:
    ClipboardSession(HWND owner, std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (OpenClipboard(owner)) {
                m_open = true;
                return;
            }
            m_error = GetLastError();
            if (std::chrono::steady_clock::now() >= deadline) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    ~ClipboardSession() {
        if (m_open) {
            CloseClipboard();
        }
    }
    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return m_open; }
    [[nodiscard]] std::string error() const {
        return "OpenClipboard failed (clipboard is in use by another application): " + detail::win32_message(m_error);
    }

private:
    bool m_open = false;
    DWORD m_error = 0;
};

/// Owns an HGLOBAL until SetClipboardData takes it.
class GlobalBuffer {
public:
    explicit GlobalBuffer(std::size_t size) noexcept
        : m_handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size == 0 ? 1 : size)) {}
    ~GlobalBuffer() {
        if (m_handle != nullptr) {
            GlobalFree(m_handle);
        }
    }
    GlobalBuffer(const GlobalBuffer&) = delete;
    GlobalBuffer& operator=(const GlobalBuffer&) = delete;

    [[nodiscard]] bool fill(const void* data, std::size_t size) noexcept {
        if (m_handle == nullptr) {
            return false;
        }
        void* dst = GlobalLock(m_handle);
        if (dst == nullptr) {
            return false;
        }
        if (size != 0) {
            std::memcpy(dst, data, size);
        }
        GlobalUnlock(m_handle);
        return true;
    }

    /// SetClipboardData; ownership passes to the system on success.
    [[nodiscard]] bool publish(UINT format) noexcept {
        if (m_handle == nullptr || SetClipboardData(format, m_handle) == nullptr) {
            return false;
        }
        m_handle = nullptr;
        return true;
    }

private:
    HGLOBAL m_handle = nullptr;
};

ClipboardResult<void> put(UINT format, const void* data, std::size_t size, std::string_view what) {
    GlobalBuffer buffer(size);
    if (!buffer.fill(data, size)) {
        return std::unexpected(detail::last_error_message(std::string("GlobalAlloc for ") + std::string(what)));
    }
    if (!buffer.publish(format)) {
        return std::unexpected(detail::last_error_message(std::string("SetClipboardData(") + std::string(what) + ")"));
    }
    return {};
}

std::optional<std::vector<std::byte>> get_bytes(UINT format) {
    HANDLE handle = GetClipboardData(format);
    if (handle == nullptr) {
        return std::nullopt;
    }
    const SIZE_T size = GlobalSize(handle);
    const void* src = GlobalLock(handle);
    if (src == nullptr) {
        return std::nullopt;
    }
    std::vector<std::byte> out(size);
    if (size != 0) {
        std::memcpy(out.data(), src, size);
    }
    GlobalUnlock(handle);
    return out;
}

std::optional<std::string> get_unicode_text() {
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        return std::nullopt;
    }
    const SIZE_T chars = GlobalSize(handle) / sizeof(wchar_t);
    const auto* src = static_cast<const wchar_t*>(GlobalLock(handle));
    if (src == nullptr) {
        return std::nullopt;
    }
    std::size_t length = 0;
    while (length < chars && src[length] != L'\0') {
        ++length;
    }
    std::string text = core::wide_to_utf8(std::wstring_view{src, length});
    GlobalUnlock(handle);
    return text;
}

ClipboardResult<void> put_text(std::string_view text) {
    std::wstring wide = core::utf8_to_wide(text);
    if (wide.empty() && !text.empty()) {
        return std::unexpected("text is not valid UTF-8");
    }
    return put(CF_UNICODETEXT, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t), "CF_UNICODETEXT");
}

ClipboardResult<void> put_files(std::span<const std::filesystem::path> files) {
    std::wstring list;
    for (const auto& path : files) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        const std::wstring native = (ec ? path : absolute).wstring();
        if (native.empty()) {
            continue;
        }
        list += native;
        list.push_back(L'\0');
    }
    list.push_back(L'\0');
    if (list.size() == 1) {
        return std::unexpected("no file paths to copy");
    }

    std::vector<std::byte> payload(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
    if (auto ok = put(CF_HDROP, payload.data(), payload.size(), "CF_HDROP"); !ok) {
        return ok;
    }
    // Explorer reads this to decide between copy and move on paste.
    constexpr DWORD kDropEffectCopy = 1; // DROPEFFECT_COPY
    return put(registered(L"Preferred DropEffect"), &kDropEffectCopy, sizeof(kDropEffectCopy), "Preferred DropEffect");
}

ClipboardResult<void> put_history_exclusion() {
    constexpr DWORD kZero = 0;
    if (auto ok = put(registered(L"ExcludeClipboardContentFromMonitorProcessing"), &kZero, sizeof(kZero),
                      "ExcludeClipboardContentFromMonitorProcessing");
        !ok) {
        return ok;
    }
    if (auto ok = put(registered(L"CanIncludeInClipboardHistory"), &kZero, sizeof(kZero), "CanIncludeInClipboardHistory"); !ok) {
        return ok;
    }
    return put(registered(L"CanUploadToCloudClipboard"), &kZero, sizeof(kZero), "CanUploadToCloudClipboard");
}

} // namespace

std::string Clipboard::format_name(std::uint32_t format) {
    for (const auto& entry : kStandardFormats) {
        if (entry.id == format) {
            return std::string(entry.name);
        }
    }
    wchar_t buffer[256]{};
    const int length = GetClipboardFormatNameW(format, buffer, static_cast<int>(std::size(buffer)));
    if (length > 0) {
        return core::wide_to_utf8(std::wstring_view{buffer, static_cast<std::size_t>(length)});
    }
    if (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST) {
        return "CF_PRIVATE_" + std::to_string(format - CF_PRIVATEFIRST);
    }
    if (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST) {
        return "CF_GDIOBJ_" + std::to_string(format - CF_GDIOBJFIRST);
    }
    return "#" + std::to_string(format);
}

std::uint32_t Clipboard::format_id(std::string_view name) noexcept {
    if (name.empty()) {
        return 0;
    }
    for (const auto& entry : kStandardFormats) {
        if (detail::iequals(entry.name, name)) {
            return entry.id;
        }
    }
    if (detail::iequals(name, "text")) return CF_UNICODETEXT;
    if (detail::iequals(name, "html")) return html_format();
    if (detail::iequals(name, "files")) return CF_HDROP;
    if (name.front() == '#') {
        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(name.data() + 1, name.data() + name.size(), value);
        if (ec == std::errc{} && ptr == name.data() + name.size()) {
            return value;
        }
    }
    try {
        const std::wstring wide = core::utf8_to_wide(name);
        return wide.empty() ? 0 : RegisterClipboardFormatW(wide.c_str());
    } catch (...) {
        return 0;
    }
}

bool Clipboard::has_format(std::string_view name) noexcept {
    if (detail::iequals(name, "image")) {
        return IsClipboardFormatAvailable(png_format()) || IsClipboardFormatAvailable(CF_DIBV5) ||
               IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP);
    }
    const std::uint32_t id = format_id(name);
    return id != 0 && IsClipboardFormatAvailable(id);
}

std::uint32_t Clipboard::sequence_number() noexcept {
    return GetClipboardSequenceNumber();
}

ClipboardResult<void> Clipboard::write(const ClipboardData& data, ClipboardWriteOptions options) const {
    // Encode before opening the clipboard: keeps the time other processes are locked out minimal.
    std::optional<std::vector<std::byte>> dib;
    if (data.png) {
        dib = png_to_dibv5(*data.png);
        if (!dib) {
            return std::unexpected("image is not a valid PNG");
        }
    }
    std::string cf_html;
    if (data.html) {
        cf_html = build_cf_html(*data.html, data.html_source_url);
    }

    ClipboardSession session(m_owner, m_open_timeout);
    if (!session.is_open()) {
        return std::unexpected(session.error());
    }
    if (!EmptyClipboard()) {
        return std::unexpected(detail::last_error_message("EmptyClipboard"));
    }
    if (data.text) {
        if (auto ok = put_text(*data.text); !ok) {
            return ok;
        }
    }
    if (data.html) {
        if (auto ok = put(html_format(), cf_html.c_str(), cf_html.size() + 1, "HTML Format"); !ok) {
            return ok;
        }
    }
    if (data.png) {
        if (auto ok = put(png_format(), data.png->data(), data.png->size(), "PNG"); !ok) {
            return ok;
        }
        if (auto ok = put(CF_DIBV5, dib->data(), dib->size(), "CF_DIBV5"); !ok) {
            return ok;
        }
    }
    if (data.files) {
        if (auto ok = put_files(*data.files); !ok) {
            return ok;
        }
    }
    if (options.exclude_from_history) {
        if (auto ok = put_history_exclusion(); !ok) {
            return ok;
        }
    }
    return {};
}

ClipboardResult<void> Clipboard::write_text(std::string_view text, ClipboardWriteOptions options) const {
    ClipboardData data;
    data.text = std::string(text);
    return write(data, options);
}

ClipboardResult<void> Clipboard::write_html(
    std::string_view html,
    std::optional<std::string_view> alt_text,
    ClipboardWriteOptions options) const {
    ClipboardData data;
    data.html = std::string(html);
    if (alt_text) {
        data.text = std::string(*alt_text);
    }
    return write(data, options);
}

ClipboardResult<void> Clipboard::write_image_png(std::span<const std::byte> png, ClipboardWriteOptions options) const {
    ClipboardData data;
    data.png = std::vector<std::byte>(png.begin(), png.end());
    return write(data, options);
}

ClipboardResult<void> Clipboard::write_files(std::span<const std::filesystem::path> files, ClipboardWriteOptions options) const {
    ClipboardData data;
    data.files = std::vector<std::filesystem::path>(files.begin(), files.end());
    return write(data, options);
}

ClipboardResult<void> Clipboard::clear() const {
    ClipboardSession session(m_owner, m_open_timeout);
    if (!session.is_open()) {
        return std::unexpected(session.error());
    }
    if (!EmptyClipboard()) {
        return std::unexpected(detail::last_error_message("EmptyClipboard"));
    }
    return {};
}

ClipboardResult<std::optional<std::string>> Clipboard::read_text() const {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return std::optional<std::string>{};
    }
    ClipboardSession session(m_owner, m_open_timeout);
    if (!session.is_open()) {
        return std::unexpected(session.error());
    }
    return get_unicode_text();
}

ClipboardResult<std::optional<CfHtml>> Clipboard::read_html() const {
    if (!IsClipboardFormatAvailable(html_format())) {
        return std::optional<CfHtml>{};
    }
    ClipboardSession session(m_owner, m_open_timeout);
    if (!session.is_open()) {
        return std::unexpected(session.error());
    }
    const auto bytes = get_bytes(html_format());
    if (!bytes) {
        return std::optional<CfHtml>{};
    }
    return parse_cf_html(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
}

ClipboardResult<std::optional<std::vector<std::byte>>> Clipboard::read_image_png() const {
    std::optional<std::vector<std::byte>> raw;
    bool is_png = false;
    {
        ClipboardSession session(m_owner, m_open_timeout);
        if (!session.is_open()) {
            return std::unexpected(session.error());
        }
        if (IsClipboardFormatAvailable(png_format())) {
            raw = get_bytes(png_format());
            is_png = raw.has_value() && png_dimensions(*raw).has_value();
        }
        if (!is_png) {
            raw.reset();
            if (IsClipboardFormatAvailable(CF_DIBV5)) {
                raw = get_bytes(CF_DIBV5);
            }
            if (!raw && IsClipboardFormatAvailable(CF_DIB)) {
                raw = get_bytes(CF_DIB);
            }
        }
    } // conversion happens after the clipboard is closed again
    if (!raw) {
        return std::optional<std::vector<std::byte>>{};
    }
    if (is_png) {
        // GlobalSize may round up; trim to the IEND chunk when present.
        static constexpr std::array<unsigned char, 8> kIend{'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
        const auto it = std::search(raw->begin(), raw->end(), kIend.begin(), kIend.end(),
                                    [](std::byte a, unsigned char b) { return std::to_integer<unsigned char>(a) == b; });
        if (it != raw->end()) {
            raw->erase(it + static_cast<std::ptrdiff_t>(kIend.size()), raw->end());
        }
        return raw;
    }
    auto png = dib_to_png(*raw);
    if (!png) {
        return std::unexpected("clipboard bitmap could not be converted to PNG");
    }
    return std::optional<std::vector<std::byte>>{std::move(*png)};
}

ClipboardResult<std::optional<std::vector<std::filesystem::path>>> Clipboard::read_files() const {
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
        return std::optional<std::vector<std::filesystem::path>>{};
    }
    ClipboardSession session(m_owner, m_open_timeout);
    if (!session.is_open()) {
        return std::unexpected(session.error());
    }
    HANDLE handle = GetClipboardData(CF_HDROP);
    if (handle == nullptr) {
        return std::optional<std::vector<std::filesystem::path>>{};
    }
    const auto drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::filesystem::path> files;
    files.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        const UINT copied = DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(copied);
        files.emplace_back(std::move(path));
    }
    return std::optional<std::vector<std::filesystem::path>>{std::move(files)};
}

ClipboardResult<std::vector<ClipboardFormatInfo>> Clipboard::formats() const {
    ClipboardSession session(m_owner, m_open_timeout);
    if (!session.is_open()) {
        return std::unexpected(session.error());
    }
    std::vector<ClipboardFormatInfo> out;
    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0) {
        out.push_back(ClipboardFormatInfo{.id = format, .name = format_name(format)});
    }
    if (const DWORD error = GetLastError(); error != ERROR_SUCCESS) {
        return std::unexpected("EnumClipboardFormats: " + detail::win32_message(error));
    }
    return out;
}

// ---- ClipboardWatcher ------------------------------------------------------------------------------------

ClipboardWatcher::ClipboardWatcher(Callback callback)
    : m_callback(std::move(callback)) {
    m_window = std::make_unique<detail::HiddenWindow>(
        [this](UINT msg, WPARAM, LPARAM) -> std::optional<LRESULT> {
            if (msg == WM_CLIPBOARDUPDATE) {
                if (m_callback) {
                    m_callback(ClipboardChange{.sequence_number = GetClipboardSequenceNumber()});
                }
                return 0;
            }
            return std::nullopt;
        },
        true);
    if (m_window->hwnd() != nullptr) {
        m_listening = AddClipboardFormatListener(m_window->hwnd()) != FALSE;
        if (!m_listening) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "AddClipboardFormatListener");
        }
    }
}

ClipboardWatcher::~ClipboardWatcher() {
    if (m_listening && m_window && m_window->hwnd() != nullptr) {
        RemoveClipboardFormatListener(m_window->hwnd());
    }
}

HWND ClipboardWatcher::hwnd() const noexcept {
    return m_window ? m_window->hwnd() : nullptr;
}

} // namespace boson::desktop
