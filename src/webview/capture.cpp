#include <boson/webview/capture.hpp>

#include "detail.hpp"

#include <shlwapi.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr std::array<std::uint8_t, 8> kPngSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::array<std::uint8_t, 3> kJpegSignature{0xFF, 0xD8, 0xFF};

COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT to_native(ImageFormat format) noexcept {
    return format == ImageFormat::Jpeg ? COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG
                                       : COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG;
}

template <std::size_t N>
bool starts_with(std::span<const std::byte> bytes, const std::array<std::uint8_t, N>& sig) noexcept {
    if (bytes.size() < N) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (std::to_integer<std::uint8_t>(bytes[i]) != sig[i]) {
            return false;
        }
    }
    return true;
}

bool start_capture(core::WebView& webview, ImageFormat format, IStream* stream, std::function<void(HRESULT)> done) {
    ICoreWebView2* core = webview.core_webview();
    if (core == nullptr || stream == nullptr) {
        return false;
    }
    return BOSON_HR_CHECK(core->CapturePreview(
        to_native(format),
        stream,
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([done = std::move(done)](HRESULT hr) -> HRESULT {
            done(hr);
            return S_OK;
        }).Get()));
}

} // namespace

std::optional<ImageFormat> parse_image_format(std::string_view name) noexcept {
    std::string lower;
    for (const char ch : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "png" || lower == "image/png") return ImageFormat::Png;
    if (lower == "jpeg" || lower == "jpg" || lower == "image/jpeg") return ImageFormat::Jpeg;
    return std::nullopt;
}

std::string_view mime_type(ImageFormat format) noexcept {
    return format == ImageFormat::Jpeg ? "image/jpeg" : "image/png";
}

bool has_image_signature(std::span<const std::byte> bytes, ImageFormat format) noexcept {
    return format == ImageFormat::Jpeg ? starts_with(bytes, kJpegSignature) : starts_with(bytes, kPngSignature);
}

std::string encode_base64(std::span<const std::byte> bytes) {
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

bool capture_preview(
    core::WebView& webview,
    ImageFormat format,
    std::function<void(HRESULT, std::vector<std::byte>)> callback) {
    ComPtr<IStream> stream;
    if (!BOSON_HR_CHECK(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        return false;
    }
    return start_capture(webview, format, stream.Get(), [stream, callback = std::move(callback)](HRESULT hr) {
        std::vector<std::byte> bytes;
        if (SUCCEEDED(hr)) {
            bytes = detail::read_stream(stream.Get());
        }
        if (callback) {
            callback(hr, std::move(bytes));
        }
    });
}

exec::CallbackSender<std::vector<std::byte>> capture_preview(core::WebView& webview, ImageFormat format) {
    return exec::CallbackSender<std::vector<std::byte>>(
        [&webview, format](exec::Completion<std::vector<std::byte>> done) {
            if (!capture_preview(webview, format, [done](HRESULT hr, std::vector<std::byte> image) { done(hr, std::move(image)); })) {
                done(E_FAIL, {});
            }
        },
        "ICoreWebView2::CapturePreview");
}

bool capture_preview_to_file(
    core::WebView& webview,
    ImageFormat format,
    std::wstring_view path,
    std::function<void(HRESULT)> callback) {
    if (path.empty() || webview.core_webview() == nullptr) {
        return false;
    }
    const std::wstring file(path);
    ComPtr<IStream> stream;
    if (!BOSON_HR_CHECK(SHCreateStreamOnFileEx(
            file.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream)) ||
        !stream) {
        return false;
    }
    return start_capture(webview, format, stream.Get(), [stream, callback = std::move(callback)](HRESULT hr) mutable {
        if (SUCCEEDED(hr)) {
            hr = stream->Commit(STGC_DEFAULT);
        }
        stream.Reset(); // close the file before reporting completion
        if (callback) {
            callback(hr);
        }
    });
}

} // namespace boson::webview
