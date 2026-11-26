#include <boson/webview/print.hpp>

#include "detail.hpp"

#include <cctype>
#include <cstdint>
#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

/// Parses a positive page number; 0 when text is not one.
std::uint64_t parse_page(std::string_view text) noexcept {
    text = trim(text);
    if (text.empty() || text.size() > 9) {
        return 0;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return 0;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

PrintStatus to_status(COREWEBVIEW2_PRINT_STATUS status) noexcept {
    switch (status) {
    case COREWEBVIEW2_PRINT_STATUS_SUCCEEDED: return PrintStatus::Succeeded;
    case COREWEBVIEW2_PRINT_STATUS_PRINTER_UNAVAILABLE: return PrintStatus::PrinterUnavailable;
    case COREWEBVIEW2_PRINT_STATUS_OTHER_ERROR: break;
    }
    return PrintStatus::OtherError;
}

COREWEBVIEW2_PRINT_ORIENTATION to_native(PrintOrientation v) noexcept {
    return v == PrintOrientation::Landscape ? COREWEBVIEW2_PRINT_ORIENTATION_LANDSCAPE : COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT;
}

COREWEBVIEW2_PRINT_COLLATION to_native(PrintCollation v) noexcept {
    switch (v) {
    case PrintCollation::Collated: return COREWEBVIEW2_PRINT_COLLATION_COLLATED;
    case PrintCollation::Uncollated: return COREWEBVIEW2_PRINT_COLLATION_UNCOLLATED;
    case PrintCollation::Default: break;
    }
    return COREWEBVIEW2_PRINT_COLLATION_DEFAULT;
}

COREWEBVIEW2_PRINT_COLOR_MODE to_native(PrintColorMode v) noexcept {
    switch (v) {
    case PrintColorMode::Color: return COREWEBVIEW2_PRINT_COLOR_MODE_COLOR;
    case PrintColorMode::Grayscale: return COREWEBVIEW2_PRINT_COLOR_MODE_GRAYSCALE;
    case PrintColorMode::Default: break;
    }
    return COREWEBVIEW2_PRINT_COLOR_MODE_DEFAULT;
}

COREWEBVIEW2_PRINT_DUPLEX to_native(PrintDuplex v) noexcept {
    switch (v) {
    case PrintDuplex::OneSided: return COREWEBVIEW2_PRINT_DUPLEX_ONE_SIDED;
    case PrintDuplex::TwoSidedLongEdge: return COREWEBVIEW2_PRINT_DUPLEX_TWO_SIDED_LONG_EDGE;
    case PrintDuplex::TwoSidedShortEdge: return COREWEBVIEW2_PRINT_DUPLEX_TWO_SIDED_SHORT_EDGE;
    case PrintDuplex::Default: break;
    }
    return COREWEBVIEW2_PRINT_DUPLEX_DEFAULT;
}

bool needs_settings2(const PrintSettings& s) noexcept {
    return s.page_ranges.has_value() || s.pages_per_side.has_value() || s.copies.has_value() ||
           s.collation.has_value() || s.color_mode.has_value() || s.duplex.has_value() || s.printer_name.has_value();
}

bool put_bool(ICoreWebView2PrintSettings* settings, const std::optional<bool>& value,
              HRESULT (STDMETHODCALLTYPE ICoreWebView2PrintSettings::*put)(BOOL), std::string_view context) {
    return !value.has_value() || core::hr_check((settings->*put)(*value ? TRUE : FALSE), context);
}

bool put_double(ICoreWebView2PrintSettings* settings, const std::optional<double>& value,
                HRESULT (STDMETHODCALLTYPE ICoreWebView2PrintSettings::*put)(double), std::string_view context) {
    return !value.has_value() || core::hr_check((settings->*put)(*value), context);
}

} // namespace

bool is_valid_page_ranges(std::string_view ranges) noexcept {
    if (trim(ranges).empty()) {
        return true;
    }
    while (true) {
        const std::size_t comma = ranges.find(',');
        const std::string_view item = trim(ranges.substr(0, comma));
        if (item.empty()) {
            return false;
        }
        const std::size_t dash = item.find('-');
        if (dash == std::string_view::npos) {
            if (parse_page(item) == 0) {
                return false;
            }
        } else {
            const std::string_view lo = trim(item.substr(0, dash));
            const std::string_view hi = trim(item.substr(dash + 1));
            if (lo.empty() && hi.empty()) {
                return false;
            }
            const std::uint64_t lo_page = lo.empty() ? 1 : parse_page(lo);
            const std::uint64_t hi_page = hi.empty() ? lo_page : parse_page(hi);
            if (lo_page == 0 || hi_page == 0 || hi_page < lo_page) {
                return false;
            }
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        ranges.remove_prefix(comma + 1);
    }
}

std::optional<std::string> validate_print_settings(const PrintSettings& s) {
    if (s.scale_factor.has_value() && (*s.scale_factor < 0.1 || *s.scale_factor > 2.0)) {
        return "scale_factor must be between 0.1 and 2.0";
    }
    if (s.page_width.has_value() && *s.page_width <= 0.0) {
        return "page_width must be greater than 0";
    }
    if (s.page_height.has_value() && *s.page_height <= 0.0) {
        return "page_height must be greater than 0";
    }
    for (const auto* margin : {&s.margin_top, &s.margin_bottom, &s.margin_left, &s.margin_right}) {
        if (margin->has_value() && **margin < 0.0) {
            return "margins must be >= 0";
        }
    }
    if (s.page_ranges.has_value() && !is_valid_page_ranges(*s.page_ranges)) {
        return "page_ranges is not a valid page range list (e.g. \"1-3, 5, 7-\")";
    }
    if (s.pages_per_side.has_value()) {
        const int v = *s.pages_per_side;
        if (v != 1 && v != 2 && v != 4 && v != 6 && v != 9 && v != 16) {
            return "pages_per_side must be one of 1, 2, 4, 6, 9, 16";
        }
    }
    if (s.copies.has_value() && (*s.copies < 1 || *s.copies > 999)) {
        return "copies must be between 1 and 999";
    }
    return std::nullopt;
}

std::string_view to_string(PrintStatus status) noexcept {
    switch (status) {
    case PrintStatus::Succeeded: return "succeeded";
    case PrintStatus::PrinterUnavailable: return "printer_unavailable";
    case PrintStatus::OtherError: break;
    }
    return "other_error";
}

std::optional<PrintOrientation> parse_print_orientation(std::string_view name) noexcept {
    if (iequals(name, "portrait")) return PrintOrientation::Portrait;
    if (iequals(name, "landscape")) return PrintOrientation::Landscape;
    return std::nullopt;
}

std::optional<PrintColorMode> parse_print_color_mode(std::string_view name) noexcept {
    if (iequals(name, "default")) return PrintColorMode::Default;
    if (iequals(name, "color")) return PrintColorMode::Color;
    if (iequals(name, "grayscale") || iequals(name, "greyscale")) return PrintColorMode::Grayscale;
    return std::nullopt;
}

std::optional<PrintDuplex> parse_print_duplex(std::string_view name) noexcept {
    if (iequals(name, "default")) return PrintDuplex::Default;
    if (iequals(name, "one_sided")) return PrintDuplex::OneSided;
    if (iequals(name, "two_sided_long_edge")) return PrintDuplex::TwoSidedLongEdge;
    if (iequals(name, "two_sided_short_edge")) return PrintDuplex::TwoSidedShortEdge;
    return std::nullopt;
}

std::optional<PrintCollation> parse_print_collation(std::string_view name) noexcept {
    if (iequals(name, "default")) return PrintCollation::Default;
    if (iequals(name, "collated")) return PrintCollation::Collated;
    if (iequals(name, "uncollated")) return PrintCollation::Uncollated;
    return std::nullopt;
}

std::optional<PrintDialogKind> parse_print_dialog_kind(std::string_view name) noexcept {
    if (iequals(name, "browser")) return PrintDialogKind::Browser;
    if (iequals(name, "system")) return PrintDialogKind::System;
    return std::nullopt;
}

ComPtr<ICoreWebView2PrintSettings> create_print_settings(ICoreWebView2Environment* env, const PrintSettings& s) {
    if (env == nullptr) {
        return nullptr;
    }
    if (const auto problem = validate_print_settings(s)) {
        core::hr_check(E_INVALIDARG, *problem);
        return nullptr;
    }
    ComPtr<ICoreWebView2Environment6> env6;
    if (!BOSON_HR_CHECK(env->QueryInterface(IID_PPV_ARGS(&env6)))) {
        return nullptr;
    }
    ComPtr<ICoreWebView2PrintSettings> out;
    if (!BOSON_HR_CHECK(env6->CreatePrintSettings(&out)) || !out) {
        return nullptr;
    }
    ICoreWebView2PrintSettings* p = out.Get();
    bool ok = true;
    if (s.orientation.has_value()) {
        ok = ok && BOSON_HR_CHECK(p->put_Orientation(to_native(*s.orientation)));
    }
    ok = ok && put_double(p, s.scale_factor, &ICoreWebView2PrintSettings::put_ScaleFactor, "PrintSettings::put_ScaleFactor");
    ok = ok && put_double(p, s.page_width, &ICoreWebView2PrintSettings::put_PageWidth, "PrintSettings::put_PageWidth");
    ok = ok && put_double(p, s.page_height, &ICoreWebView2PrintSettings::put_PageHeight, "PrintSettings::put_PageHeight");
    ok = ok && put_double(p, s.margin_top, &ICoreWebView2PrintSettings::put_MarginTop, "PrintSettings::put_MarginTop");
    ok = ok && put_double(p, s.margin_bottom, &ICoreWebView2PrintSettings::put_MarginBottom, "PrintSettings::put_MarginBottom");
    ok = ok && put_double(p, s.margin_left, &ICoreWebView2PrintSettings::put_MarginLeft, "PrintSettings::put_MarginLeft");
    ok = ok && put_double(p, s.margin_right, &ICoreWebView2PrintSettings::put_MarginRight, "PrintSettings::put_MarginRight");
    ok = ok && put_bool(p, s.print_backgrounds, &ICoreWebView2PrintSettings::put_ShouldPrintBackgrounds, "PrintSettings::put_ShouldPrintBackgrounds");
    ok = ok && put_bool(p, s.selection_only, &ICoreWebView2PrintSettings::put_ShouldPrintSelectionOnly, "PrintSettings::put_ShouldPrintSelectionOnly");
    ok = ok && put_bool(p, s.header_and_footer, &ICoreWebView2PrintSettings::put_ShouldPrintHeaderAndFooter, "PrintSettings::put_ShouldPrintHeaderAndFooter");
    if (s.header_title.has_value()) {
        const std::wstring wide = core::utf8_to_wide(*s.header_title);
        ok = ok && BOSON_HR_CHECK(p->put_HeaderTitle(wide.c_str()));
    }
    if (s.footer_uri.has_value()) {
        const std::wstring wide = core::utf8_to_wide(*s.footer_uri);
        ok = ok && BOSON_HR_CHECK(p->put_FooterUri(wide.c_str()));
    }

    if (ok && needs_settings2(s)) {
        ComPtr<ICoreWebView2PrintSettings2> p2;
        if (!BOSON_HR_CHECK(out.As(&p2))) {
            return nullptr;
        }
        if (s.page_ranges.has_value()) {
            const std::wstring wide = core::utf8_to_wide(trim(*s.page_ranges));
            ok = ok && BOSON_HR_CHECK(p2->put_PageRanges(wide.c_str()));
        }
        if (s.pages_per_side.has_value()) {
            ok = ok && BOSON_HR_CHECK(p2->put_PagesPerSide(*s.pages_per_side));
        }
        if (s.copies.has_value()) {
            ok = ok && BOSON_HR_CHECK(p2->put_Copies(*s.copies));
        }
        if (s.collation.has_value()) {
            ok = ok && BOSON_HR_CHECK(p2->put_Collation(to_native(*s.collation)));
        }
        if (s.color_mode.has_value()) {
            ok = ok && BOSON_HR_CHECK(p2->put_ColorMode(to_native(*s.color_mode)));
        }
        if (s.duplex.has_value()) {
            ok = ok && BOSON_HR_CHECK(p2->put_Duplex(to_native(*s.duplex)));
        }
        if (s.printer_name.has_value()) {
            const std::wstring wide = core::utf8_to_wide(*s.printer_name);
            ok = ok && BOSON_HR_CHECK(p2->put_PrinterName(wide.c_str()));
        }
    }
    return ok ? out : nullptr;
}

bool show_print_ui(core::WebView& webview, PrintDialogKind kind) noexcept {
    const auto webview16 = detail::require<ICoreWebView2_16>(webview, "ICoreWebView2_16 (ShowPrintUI)");
    if (!webview16) {
        return false;
    }
    return BOSON_HR_CHECK(webview16->ShowPrintUI(
        kind == PrintDialogKind::System ? COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM : COREWEBVIEW2_PRINT_DIALOG_KIND_BROWSER));
}

bool print(core::WebView& webview, const PrintSettings& settings, std::function<void(HRESULT, PrintStatus)> callback) {
    const auto webview16 = detail::require<ICoreWebView2_16>(webview, "ICoreWebView2_16 (Print)");
    if (!webview16) {
        return false;
    }
    const auto native = create_print_settings(detail::environment_of(webview).Get(), settings);
    if (!native) {
        return false;
    }
    return BOSON_HR_CHECK(webview16->Print(
        native.Get(),
        Callback<ICoreWebView2PrintCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, COREWEBVIEW2_PRINT_STATUS status) -> HRESULT {
                if (callback) {
                    callback(hr, SUCCEEDED(hr) ? to_status(status) : PrintStatus::OtherError);
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<PrintStatus> print(core::WebView& webview, PrintSettings settings) {
    return exec::CallbackSender<PrintStatus>(
        [&webview, settings = std::move(settings)](exec::Completion<PrintStatus> done) {
            if (!print(webview, settings, [done](HRESULT hr, PrintStatus status) { done(hr, status); })) {
                done(E_FAIL, PrintStatus::OtherError);
            }
        },
        "ICoreWebView2_16::Print");
}

bool print_to_pdf(
    core::WebView& webview,
    std::wstring_view path,
    const PrintSettings& settings,
    std::function<void(HRESULT, bool)> callback) {
    if (path.empty()) {
        core::hr_check(E_INVALIDARG, "print_to_pdf: empty path");
        return false;
    }
    const auto webview7 = detail::require<ICoreWebView2_7>(webview, "ICoreWebView2_7 (PrintToPdf)");
    if (!webview7) {
        return false;
    }
    const auto native = create_print_settings(detail::environment_of(webview).Get(), settings);
    if (!native) {
        return false;
    }
    const std::wstring file(path);
    return BOSON_HR_CHECK(webview7->PrintToPdf(
        file.c_str(),
        native.Get(),
        Callback<ICoreWebView2PrintToPdfCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, BOOL ok) -> HRESULT {
                if (callback) {
                    callback(hr, SUCCEEDED(hr) && ok != FALSE);
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<bool> print_to_pdf(core::WebView& webview, std::wstring path, PrintSettings settings) {
    return exec::CallbackSender<bool>(
        [&webview, path = std::move(path), settings = std::move(settings)](exec::Completion<bool> done) {
            if (!print_to_pdf(webview, path, settings, [done](HRESULT hr, bool ok) { done(hr, ok); })) {
                done(E_FAIL, false);
            }
        },
        "ICoreWebView2_7::PrintToPdf");
}

bool print_to_pdf_stream(
    core::WebView& webview,
    const PrintSettings& settings,
    std::function<void(HRESULT, std::vector<std::byte>)> callback) {
    const auto webview16 = detail::require<ICoreWebView2_16>(webview, "ICoreWebView2_16 (PrintToPdfStream)");
    if (!webview16) {
        return false;
    }
    const auto native = create_print_settings(detail::environment_of(webview).Get(), settings);
    if (!native) {
        return false;
    }
    return BOSON_HR_CHECK(webview16->PrintToPdfStream(
        native.Get(),
        Callback<ICoreWebView2PrintToPdfStreamCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, IStream* stream) -> HRESULT {
                std::vector<std::byte> bytes;
                if (SUCCEEDED(hr)) {
                    bytes = detail::read_stream(stream);
                }
                if (callback) {
                    callback(hr, std::move(bytes));
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<std::vector<std::byte>> print_to_pdf_stream(core::WebView& webview, PrintSettings settings) {
    return exec::CallbackSender<std::vector<std::byte>>(
        [&webview, settings = std::move(settings)](exec::Completion<std::vector<std::byte>> done) {
            if (!print_to_pdf_stream(webview, settings, [done](HRESULT hr, std::vector<std::byte> pdf) { done(hr, std::move(pdf)); })) {
                done(E_FAIL, {});
            }
        },
        "ICoreWebView2_16::PrintToPdfStream");
}

} // namespace boson::webview
