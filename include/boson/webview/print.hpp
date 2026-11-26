#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/callback_sender.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview {

enum class PrintOrientation {
    Portrait,
    Landscape,
};

enum class PrintCollation {
    Default,
    Collated,
    Uncollated,
};

enum class PrintColorMode {
    Default,
    Color,
    Grayscale,
};

enum class PrintDuplex {
    Default,
    OneSided,
    TwoSidedLongEdge,
    TwoSidedShortEdge,
};

enum class PrintDialogKind {
    Browser, ///< The Edge print preview dialog.
    System,  ///< The Windows system print dialog.
};

enum class PrintStatus {
    Succeeded,
    PrinterUnavailable,
    OtherError,
};

/// @brief ICoreWebView2PrintSettings / ICoreWebView2PrintSettings2. Unset fields keep the runtime default.
/// Lengths are in inches, like the WebView2 API.
struct PrintSettings {
    std::optional<PrintOrientation> orientation;
    std::optional<double> scale_factor; ///< 0.1 .. 2.0
    std::optional<double> page_width;   ///< > 0
    std::optional<double> page_height;  ///< > 0
    std::optional<double> margin_top;   ///< >= 0
    std::optional<double> margin_bottom;
    std::optional<double> margin_left;
    std::optional<double> margin_right;
    std::optional<bool> print_backgrounds;
    std::optional<bool> selection_only;
    std::optional<bool> header_and_footer;
    std::optional<std::string> header_title; ///< UTF-8; shown when header_and_footer is true.
    std::optional<std::string> footer_uri;   ///< UTF-8; shown when header_and_footer is true.
    // ICoreWebView2PrintSettings2 (runtime 1.0.1518+). Setting any of these on an older runtime fails.
    std::optional<std::string> page_ranges;  ///< e.g. "1-3, 5, 7-"; empty = all pages.
    std::optional<int> pages_per_side;       ///< 1, 2, 4, 6, 9 or 16
    std::optional<int> copies;               ///< 1 .. 999 (Print only)
    std::optional<PrintCollation> collation; ///< Print only
    std::optional<PrintColorMode> color_mode;
    std::optional<PrintDuplex> duplex;       ///< Print only
    std::optional<std::string> printer_name; ///< Print only; empty/unset = default printer.
};

/// @brief Checks ranges and syntax before anything reaches COM. Returns the first problem, or nullopt.
[[nodiscard]] std::optional<std::string> validate_print_settings(const PrintSettings& settings);

/// @brief Syntax of ICoreWebView2PrintSettings2::PageRanges: comma separated "N", "N-M", "N-" or "-M"
/// items with 1-based page numbers and N <= M. An empty (or all-whitespace) string means all pages.
[[nodiscard]] bool is_valid_page_ranges(std::string_view ranges) noexcept;

[[nodiscard]] std::string_view to_string(PrintStatus status) noexcept;
[[nodiscard]] std::optional<PrintOrientation> parse_print_orientation(std::string_view name) noexcept;
[[nodiscard]] std::optional<PrintColorMode> parse_print_color_mode(std::string_view name) noexcept;
[[nodiscard]] std::optional<PrintDuplex> parse_print_duplex(std::string_view name) noexcept;
[[nodiscard]] std::optional<PrintCollation> parse_print_collation(std::string_view name) noexcept;
[[nodiscard]] std::optional<PrintDialogKind> parse_print_dialog_kind(std::string_view name) noexcept;

/// @brief Builds a COM print settings object (ICoreWebView2Environment6::CreatePrintSettings). Null when
/// the settings are invalid (logged) or the runtime lacks a needed interface.
[[nodiscard]] Microsoft::WRL::ComPtr<ICoreWebView2PrintSettings> create_print_settings(
    ICoreWebView2Environment* env,
    const PrintSettings& settings);

// All functions below must be called on the UI thread; callbacks run on the UI thread. They return false
// (and never invoke the callback) when the operation could not be started.

/// @brief ICoreWebView2_16::ShowPrintUI. Opens a modal-less print dialog owned by the WebView.
bool show_print_ui(core::WebView& webview, PrintDialogKind kind = PrintDialogKind::Browser) noexcept;

/// @brief Silent print (ICoreWebView2_16::Print) to settings.printer_name or the default printer.
bool print(core::WebView& webview, const PrintSettings& settings, std::function<void(HRESULT, PrintStatus)> callback);
[[nodiscard]] exec::CallbackSender<PrintStatus> print(core::WebView& webview, PrintSettings settings);

/// @brief ICoreWebView2_7::PrintToPdf; callback receives true when the file was written.
bool print_to_pdf(
    core::WebView& webview,
    std::wstring_view path,
    const PrintSettings& settings,
    std::function<void(HRESULT, bool)> callback);
[[nodiscard]] exec::CallbackSender<bool> print_to_pdf(core::WebView& webview, std::wstring path, PrintSettings settings);

/// @brief ICoreWebView2_16::PrintToPdfStream; callback receives the PDF bytes.
bool print_to_pdf_stream(
    core::WebView& webview,
    const PrintSettings& settings,
    std::function<void(HRESULT, std::vector<std::byte>)> callback);
[[nodiscard]] exec::CallbackSender<std::vector<std::byte>> print_to_pdf_stream(core::WebView& webview, PrintSettings settings);

} // namespace boson::webview
