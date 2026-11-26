#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/callback_sender.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview {

enum class ImageFormat {
    Png,
    Jpeg,
};

[[nodiscard]] std::optional<ImageFormat> parse_image_format(std::string_view name) noexcept;
[[nodiscard]] std::string_view mime_type(ImageFormat format) noexcept;

/// @brief true when bytes start with the PNG / JPEG file signature of format.
[[nodiscard]] bool has_image_signature(std::span<const std::byte> bytes, ImageFormat format) noexcept;

/// @brief Standard (RFC 4648, padded) base64, used to move binary results over the JSON bridge.
[[nodiscard]] std::string encode_base64(std::span<const std::byte> bytes);

/// @brief ICoreWebView2::CapturePreview of the visible viewport into memory. UI thread; callback on the
/// UI thread. Returns false (callback not invoked) when the capture could not be started.
bool capture_preview(
    core::WebView& webview,
    ImageFormat format,
    std::function<void(HRESULT, std::vector<std::byte>)> callback);
[[nodiscard]] exec::CallbackSender<std::vector<std::byte>> capture_preview(core::WebView& webview, ImageFormat format);

/// @brief Like capture_preview, but streams straight into a file (created or truncated).
bool capture_preview_to_file(
    core::WebView& webview,
    ImageFormat format,
    std::wstring_view path,
    std::function<void(HRESULT)> callback);

} // namespace boson::webview
