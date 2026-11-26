#pragma once

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::desktop {

namespace detail {
class HiddenWindow;
} // namespace detail

// ---- Pure helpers (no clipboard access; unit-testable) ---------------------------------------------------

/// @brief Parsed CF_HTML ("HTML Format") payload. All strings are UTF-8.
struct CfHtml {
    std::string html;       ///< Full HTML document (StartHTML..EndHTML).
    std::string fragment;   ///< Selected fragment (StartFragment..EndFragment).
    std::string source_url; ///< SourceURL header, empty when absent.
};

/// @brief Builds a CF_HTML payload with exact byte offsets. fragment_html is wrapped in
/// <html><body><!--StartFragment-->...<!--EndFragment--></body></html>.
[[nodiscard]] std::string build_cf_html(std::string_view fragment_html, std::string_view source_url = {});
/// @brief Parses a CF_HTML payload (tolerates a trailing NUL and missing optional headers).
[[nodiscard]] std::optional<CfHtml> parse_cf_html(std::string_view cf_html);

[[nodiscard]] std::string base64_encode(std::span<const std::byte> bytes);
/// @brief Standard alphabet; whitespace is ignored, padding optional. nullopt on invalid input.
[[nodiscard]] std::optional<std::vector<std::byte>> base64_decode(std::string_view text);

/// @brief 32-bit BGRA image, rows top-down, straight (non-premultiplied) alpha.
struct BgraImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> pixels; ///< width * height * 4 bytes
};

/// @brief WIC PNG decode / encode. Initialise COM on the calling thread is not required.
[[nodiscard]] std::optional<BgraImage> decode_png(std::span<const std::byte> png);
[[nodiscard]] std::optional<std::vector<std::byte>> encode_png(const BgraImage& image);
/// @brief Width/height from the PNG IHDR chunk without decoding.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> png_dimensions(std::span<const std::byte> png) noexcept;

/// @brief PNG -> packed CF_DIBV5 (BITMAPV5HEADER, 32bpp BI_BITFIELDS with alpha mask, bottom-up).
[[nodiscard]] std::optional<std::vector<std::byte>> png_to_dibv5(std::span<const std::byte> png);
/// @brief Packed DIB (CF_DIB or CF_DIBV5: BITMAPINFOHEADER/V4/V5, any bit depth WIC can read) -> PNG.
[[nodiscard]] std::optional<std::vector<std::byte>> dib_to_png(std::span<const std::byte> dib);

// ---- System clipboard ------------------------------------------------------------------------------------

struct ClipboardFormatInfo {
    std::uint32_t id = 0;
    std::string name; ///< "CF_UNICODETEXT", "HTML Format", "PNG", ...
};

/// @brief Everything one write() puts on the clipboard in a single transaction.
struct ClipboardData {
    std::optional<std::string> text;                       ///< CF_UNICODETEXT
    std::optional<std::string> html;                       ///< CF_HTML fragment
    std::string html_source_url;                           ///< SourceURL header of the CF_HTML entry
    std::optional<std::vector<std::byte>> png;             ///< "PNG" + CF_DIBV5
    std::optional<std::vector<std::filesystem::path>> files; ///< CF_HDROP + "Preferred DropEffect" (copy)
};

struct ClipboardWriteOptions {
    /// Adds "ExcludeClipboardContentFromMonitorProcessing", "CanIncludeInClipboardHistory" = 0 and
    /// "CanUploadToCloudClipboard" = 0, so Win+V history, cloud sync and clipboard monitors skip it
    /// (passwords, tokens).
    bool exclude_from_history = false;
};

template <typename T>
using ClipboardResult = std::expected<T, std::string>;

/// @brief System clipboard access. Safe on any thread (each call opens and closes the clipboard), but it
/// may wait up to open_timeout for another process to close the clipboard, so call it from a TaskRunner
/// thread rather than the UI thread. owner should be a window whose thread pumps messages (the main
/// window); nullptr works for writes on current Windows but leaves the clipboard without an owner.
class Clipboard {
public:
    explicit Clipboard(HWND owner = nullptr, std::chrono::milliseconds open_timeout = std::chrono::milliseconds{500}) noexcept
        : m_owner(owner), m_open_timeout(open_timeout) {}

    [[nodiscard]] ClipboardResult<void> write(const ClipboardData& data, ClipboardWriteOptions options = {}) const;
    [[nodiscard]] ClipboardResult<void> write_text(std::string_view text, ClipboardWriteOptions options = {}) const;
    /// @brief CF_HTML plus, when alt_text is given, CF_UNICODETEXT in the same transaction.
    [[nodiscard]] ClipboardResult<void> write_html(
        std::string_view html,
        std::optional<std::string_view> alt_text = std::nullopt,
        ClipboardWriteOptions options = {}) const;
    [[nodiscard]] ClipboardResult<void> write_image_png(std::span<const std::byte> png, ClipboardWriteOptions options = {}) const;
    [[nodiscard]] ClipboardResult<void> write_files(std::span<const std::filesystem::path> files, ClipboardWriteOptions options = {}) const;
    [[nodiscard]] ClipboardResult<void> clear() const;

    /// Each read returns std::nullopt (not an error) when the clipboard holds no data of that kind.
    [[nodiscard]] ClipboardResult<std::optional<std::string>> read_text() const;
    [[nodiscard]] ClipboardResult<std::optional<CfHtml>> read_html() const;
    /// @brief "PNG" when present (keeps alpha), else CF_DIBV5 / CF_DIB converted to PNG.
    [[nodiscard]] ClipboardResult<std::optional<std::vector<std::byte>>> read_image_png() const;
    [[nodiscard]] ClipboardResult<std::optional<std::vector<std::filesystem::path>>> read_files() const;
    [[nodiscard]] ClipboardResult<std::vector<ClipboardFormatInfo>> formats() const;

    /// @brief IsClipboardFormatAvailable for a CF_* name ("CF_UNICODETEXT"), a registered name ("PNG") or
    /// the aliases "text", "html", "image", "files". Does not open the clipboard.
    [[nodiscard]] static bool has_format(std::string_view name) noexcept;
    [[nodiscard]] static std::uint32_t sequence_number() noexcept;
    [[nodiscard]] static std::string format_name(std::uint32_t format);
    /// @brief Inverse of format_name(); registers unknown names. 0 when name is empty.
    [[nodiscard]] static std::uint32_t format_id(std::string_view name) noexcept;

private:
    HWND m_owner = nullptr;
    std::chrono::milliseconds m_open_timeout;
};

struct ClipboardChange {
    std::uint32_t sequence_number = 0;
};

/// @brief AddClipboardFormatListener on a private message-only window. The callback runs on the thread
/// that constructed the watcher (the UI thread), once per clipboard change by any process.
class ClipboardWatcher {
public:
    using Callback = std::function<void(const ClipboardChange& change)>;

    explicit ClipboardWatcher(Callback callback);
    ~ClipboardWatcher();
    ClipboardWatcher(const ClipboardWatcher&) = delete;
    ClipboardWatcher& operator=(const ClipboardWatcher&) = delete;

    [[nodiscard]] bool is_active() const noexcept { return m_listening; }
    [[nodiscard]] HWND hwnd() const noexcept;

private:
    Callback m_callback;
    std::unique_ptr<detail::HiddenWindow> m_window;
    bool m_listening = false;
};

} // namespace boson::desktop
