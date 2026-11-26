#pragma once

#include <windows.h>

#include <boson/core/result.hpp>
#include <boson/desktop/clipboard.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/controllers/desktop_events.hpp>
#include <boson/ipc/export.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace boson::ipc {

struct ClipboardTextRequest {
    std::string text;
    bool exclude_from_history = false;
};

struct ClipboardTextResponse {
    std::optional<std::string> text; ///< null when the clipboard holds no text
};

struct ClipboardHtmlRequest {
    std::string html;                ///< fragment, wrapped into a CF_HTML document
    std::optional<std::string> text; ///< plain-text alternative written in the same transaction
    std::string source_url;
    bool exclude_from_history = false;
};

struct ClipboardHtmlResponse {
    bool available = false;
    std::string html;
    std::string fragment;
    std::string source_url;
};

struct ClipboardImageRequest {
    std::string png_base64; ///< raw base64 or a data:image/png;base64,... URL
    bool exclude_from_history = false;
};

struct ClipboardImageResponse {
    bool available = false;
    std::string png_base64;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ClipboardFilesRequest {
    std::vector<std::string> paths;
};

struct ClipboardFilesResponse {
    std::vector<std::string> paths;
};

struct ClipboardFormatEntry {
    std::uint32_t id = 0;
    std::string name;
};

struct ClipboardFormatsResponse {
    std::uint32_t sequence_number = 0;
    std::vector<ClipboardFormatEntry> formats;
};

struct ClipboardHasRequest {
    std::string format; ///< "text" | "html" | "image" | "files" | "CF_*" | registered name
};

struct ClipboardBoolResponse {
    bool value = false;
};

/// @brief JS "Clipboard.*". Runs on the router's pool thread; the clipboard is opened per call.
class ClipboardController {
public:
    BOSON_CONTROLLER("Clipboard");

    explicit ClipboardController(HWND owner = nullptr) noexcept
        : m_clipboard(owner) {}

    [[nodiscard]] ClipboardTextResponse read_text() const {
        return ClipboardTextResponse{.text = unwrap(m_clipboard.read_text())};
    }

    void write_text(ClipboardTextRequest req) const {
        check(m_clipboard.write_text(req.text, {.exclude_from_history = req.exclude_from_history}));
    }

    [[nodiscard]] ClipboardHtmlResponse read_html() const {
        const auto html = unwrap(m_clipboard.read_html());
        if (!html) {
            return {};
        }
        return ClipboardHtmlResponse{.available = true, .html = html->html, .fragment = html->fragment, .source_url = html->source_url};
    }

    void write_html(ClipboardHtmlRequest req) const {
        desktop::ClipboardData data;
        data.html = std::move(req.html);
        data.html_source_url = std::move(req.source_url);
        data.text = std::move(req.text);
        check(m_clipboard.write(data, {.exclude_from_history = req.exclude_from_history}));
    }

    [[nodiscard]] ClipboardImageResponse read_image() const {
        const auto png = unwrap(m_clipboard.read_image_png());
        if (!png) {
            return {};
        }
        const auto size = desktop::png_dimensions(*png);
        return ClipboardImageResponse{
            .available = true,
            .png_base64 = desktop::base64_encode(*png),
            .width = size ? size->first : 0,
            .height = size ? size->second : 0,
        };
    }

    void write_image(ClipboardImageRequest req) const {
        const auto png = desktop::base64_decode(req.png_base64);
        if (!png || png->empty()) {
            throw std::invalid_argument("png_base64 is not valid base64");
        }
        check(m_clipboard.write_image_png(*png, {.exclude_from_history = req.exclude_from_history}));
    }

    [[nodiscard]] ClipboardFilesResponse read_files() const {
        ClipboardFilesResponse out;
        if (const auto files = unwrap(m_clipboard.read_files())) {
            for (const auto& path : *files) {
                out.paths.push_back(core::wide_to_utf8(path.wstring()));
            }
        }
        return out;
    }

    void write_files(ClipboardFilesRequest req) const {
        std::vector<std::filesystem::path> files;
        for (const auto& path : req.paths) {
            files.emplace_back(core::utf8_to_wide(path));
        }
        check(m_clipboard.write_files(files));
    }

    void clear() const { check(m_clipboard.clear()); }

    [[nodiscard]] ClipboardFormatsResponse formats() const {
        ClipboardFormatsResponse out{.sequence_number = desktop::Clipboard::sequence_number()};
        for (auto& format : unwrap(m_clipboard.formats())) {
            out.formats.push_back(ClipboardFormatEntry{.id = format.id, .name = std::move(format.name)});
        }
        return out;
    }

    [[nodiscard]] ClipboardBoolResponse has(ClipboardHasRequest req) const {
        return ClipboardBoolResponse{.value = desktop::Clipboard::has_format(req.format)};
    }

private:
    template <typename T>
    static T unwrap(desktop::ClipboardResult<T> result) {
        if (!result) {
            throw std::runtime_error(result.error());
        }
        return std::move(*result);
    }

    static void check(const desktop::ClipboardResult<void>& result) {
        if (!result) {
            throw std::runtime_error(result.error());
        }
    }

    desktop::Clipboard m_clipboard;
};

/// @brief Emits "clipboard:changed" to every WebView attached to bridge for as long as the returned
/// watcher lives. Call on the UI thread.
[[nodiscard]] inline std::unique_ptr<desktop::ClipboardWatcher> watch_clipboard(Bridge& bridge) {
    return std::make_unique<desktop::ClipboardWatcher>([&bridge](const desktop::ClipboardChange& change) {
        bridge.emit("clipboard:changed", ClipboardChangedEvent{
            .sequence_number = change.sequence_number,
            .has_text = desktop::Clipboard::has_format("text"),
            .has_html = desktop::Clipboard::has_format("html"),
            .has_image = desktop::Clipboard::has_format("image"),
            .has_files = desktop::Clipboard::has_format("files"),
        });
    });
}

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::ClipboardController> {
    using T = boson::ipc::ClipboardController;
    static constexpr auto value = glz::object(
        "read_text", &T::read_text,
        "write_text", &T::write_text,
        "read_html", &T::read_html,
        "write_html", &T::write_html,
        "read_image", &T::read_image,
        "write_image", &T::write_image,
        "read_files", &T::read_files,
        "write_files", &T::write_files,
        "clear", &T::clear,
        "formats", &T::formats,
        "has", &T::has);
};
