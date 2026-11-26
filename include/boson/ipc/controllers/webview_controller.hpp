#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/export.hpp>
#include <boson/webview/capture.hpp>
#include <boson/webview/cdp.hpp>
#include <boson/webview/cookies.hpp>
#include <boson/webview/drm.hpp>
#include <boson/webview/find.hpp>
#include <boson/webview/print.hpp>
#include <boson/webview/profile.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::ipc {

/// JSON form of webview::PrintSettings; enum fields are strings ("landscape", "grayscale", ...).
struct PrintOptions {
    std::optional<std::string> orientation;   ///< "portrait" | "landscape"
    std::optional<double> scale_factor;
    std::optional<double> page_width;         ///< inches
    std::optional<double> page_height;
    std::optional<double> margin_top;
    std::optional<double> margin_bottom;
    std::optional<double> margin_left;
    std::optional<double> margin_right;
    std::optional<bool> print_backgrounds;
    std::optional<bool> selection_only;
    std::optional<bool> header_and_footer;
    std::optional<std::string> header_title;
    std::optional<std::string> footer_uri;
    std::optional<std::string> page_ranges;   ///< "1-3, 5"
    std::optional<int> pages_per_side;
    std::optional<int> copies;
    std::optional<std::string> collation;     ///< "default" | "collated" | "uncollated"
    std::optional<std::string> color_mode;    ///< "default" | "color" | "grayscale"
    std::optional<std::string> duplex;        ///< "default" | "one_sided" | "two_sided_long_edge" | "two_sided_short_edge"
    std::optional<std::string> printer_name;
};

struct PrintUiRequest {
    std::string kind = "browser"; ///< "browser" | "system"
};

struct PrintResult {
    bool ok = false;
    std::string status; ///< "succeeded" | "printer_unavailable" | "other_error"
};

struct PrintToPdfRequest {
    std::string path; ///< UTF-8 absolute file path
    PrintOptions settings;
};

struct PdfResult {
    bool ok = false;
    std::string path;      ///< print_to_pdf only
    std::string data;      ///< print_to_pdf_data only: base64
    std::uint64_t size = 0;
};

struct CaptureRequest {
    std::string format = "png"; ///< "png" | "jpeg"
    std::optional<std::string> path; ///< When set, written to this file instead of returned.
};

struct CaptureResult {
    std::string mime_type;
    std::string data; ///< base64 (empty when written to path)
    std::string path;
    std::uint64_t size = 0;
};

struct FindRequest {
    std::string term;
    bool case_sensitive = false;
    bool whole_word = false;
    bool highlight_all = true;
    bool suppress_default_dialog = true;
};

struct FindStatus {
    std::int32_t match_count = 0;
    std::int32_t active_match_index = -1; ///< 1-based; -1 = none
    bool active = false;
};

struct CookieData {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    std::optional<double> expires; ///< UNIX seconds; null = session cookie
    bool http_only = false;
    bool secure = false;
    std::string same_site = "lax"; ///< "none" | "lax" | "strict"
    bool is_session = true;        ///< read-only
};

struct CookieQuery {
    std::string uri; ///< empty = every cookie of the profile
};

struct DeleteCookiesRequest {
    std::string name;
    std::optional<std::string> uri;    ///< delete by name + uri, or
    std::optional<std::string> domain; ///< by name + domain + path
    std::optional<std::string> path;
};

struct ClearDataRequest {
    /// "cookies", "disk_cache", "local_storage", "indexed_db", "cache_storage", "all_site", "all_profile", ...
    /// Empty = everything.
    std::vector<std::string> kinds;
    std::optional<double> start_time; ///< UNIX seconds
    std::optional<double> end_time;
};

struct ProfileData {
    std::string name;
    bool in_private = false;
    std::string path;
    std::string default_download_folder;
    std::string color_scheme;
    std::optional<std::string> tracking_prevention;
};

struct ColorSchemeRequest {
    std::string scheme = "auto"; ///< "auto" | "light" | "dark"
};

struct OkResponse {
    bool ok = false;
};

/// WebView.cdp_send: one Chrome DevTools Protocol command on the calling WebView.
struct CdpCommand {
    std::string method;
    glz::raw_json params{"{}"};
    std::string session_id; ///< Flattened child session (OOPIF / worker); empty = the page.
};

struct CdpCommandResult {
    glz::raw_json result{"{}"};
};

struct CdpSubscription {
    std::vector<std::string> events; ///< "Network.requestWillBeSent", ...
    bool subscribe = true;
};

/// "cdp:event" payload (WebViewController::on_cdp_event).
struct CdpEventPayload {
    std::string method;
    glz::raw_json params{"{}"};
    std::string session_id;
};

/// JSON form of webview::DrmProbeOptions. Omitted fields keep the native defaults. (Optional vectors so that
/// a list sent by JS replaces the default list instead of being merged into its elements.)
struct DrmProbeRequest {
    std::optional<std::vector<webview::DrmKeySystemQuery>> key_systems;
    std::optional<std::vector<webview::DrmCodec>> video_codecs;
    std::optional<std::vector<webview::DrmCodec>> audio_codecs;
    std::optional<std::vector<std::string>> encryption_schemes;
    std::optional<bool> create_media_keys;
    std::optional<bool> check_hdcp;
    std::optional<std::uint32_t> timeout_ms;
};

/// @brief Applies a request on top of the defaults; throws std::invalid_argument on out-of-range values.
[[nodiscard]] inline webview::DrmProbeOptions to_drm_probe_options(DrmProbeRequest req) {
    webview::DrmProbeOptions o;
    if (req.key_systems) {
        o.key_systems = std::move(*req.key_systems);
    }
    if (req.video_codecs) {
        o.video_codecs = std::move(*req.video_codecs);
    }
    if (req.audio_codecs) {
        o.audio_codecs = std::move(*req.audio_codecs);
    }
    if (req.encryption_schemes) {
        o.encryption_schemes = std::move(*req.encryption_schemes);
    }
    o.create_media_keys = req.create_media_keys.value_or(o.create_media_keys);
    o.check_hdcp = req.check_hdcp.value_or(o.check_hdcp);
    o.timeout_ms = req.timeout_ms.value_or(o.timeout_ms);
    if (o.timeout_ms == 0 || o.timeout_ms > 60000) {
        throw std::invalid_argument("timeout_ms must be between 1 and 60000");
    }
    if (o.key_systems.empty() || o.key_systems.size() > 32) {
        throw std::invalid_argument("key_systems must list between 1 and 32 queries");
    }
    if (o.video_codecs.empty()) {
        throw std::invalid_argument("video_codecs must not be empty");
    }
    for (const auto& q : o.key_systems) {
        if (q.key_system.empty()) {
            throw std::invalid_argument("key_system must not be empty");
        }
    }
    for (const auto& c : o.video_codecs) {
        if (c.content_type.empty()) {
            throw std::invalid_argument("video_codecs[].content_type must not be empty");
        }
    }
    for (const auto& c : o.audio_codecs) {
        if (c.content_type.empty()) {
            throw std::invalid_argument("audio_codecs[].content_type must not be empty");
        }
    }
    return o;
}

/// @brief Converts the JSON print options; throws std::invalid_argument on unknown names or invalid values.
[[nodiscard]] inline webview::PrintSettings to_print_settings(const PrintOptions& o) {
    webview::PrintSettings s;
    const auto pick = [](const std::optional<std::string>& name, auto parse, std::string_view what) {
        using Result = decltype(parse(std::string_view{}));
        if (!name.has_value()) {
            return Result{};
        }
        Result value = parse(*name);
        if (!value.has_value()) {
            throw std::invalid_argument("Unsupported " + std::string(what) + ": " + *name);
        }
        return value;
    };
    s.orientation = pick(o.orientation, &webview::parse_print_orientation, "orientation");
    s.collation = pick(o.collation, &webview::parse_print_collation, "collation");
    s.color_mode = pick(o.color_mode, &webview::parse_print_color_mode, "color_mode");
    s.duplex = pick(o.duplex, &webview::parse_print_duplex, "duplex");
    s.scale_factor = o.scale_factor;
    s.page_width = o.page_width;
    s.page_height = o.page_height;
    s.margin_top = o.margin_top;
    s.margin_bottom = o.margin_bottom;
    s.margin_left = o.margin_left;
    s.margin_right = o.margin_right;
    s.print_backgrounds = o.print_backgrounds;
    s.selection_only = o.selection_only;
    s.header_and_footer = o.header_and_footer;
    s.header_title = o.header_title;
    s.footer_uri = o.footer_uri;
    s.page_ranges = o.page_ranges;
    s.pages_per_side = o.pages_per_side;
    s.copies = o.copies;
    s.printer_name = o.printer_name;
    if (const auto problem = webview::validate_print_settings(s)) {
        throw std::invalid_argument(*problem);
    }
    return s;
}

[[nodiscard]] inline CookieData to_cookie_data(const webview::Cookie& c) {
    return CookieData{
        .name = c.name,
        .value = c.value,
        .domain = c.domain,
        .path = c.path,
        .expires = c.expires,
        .http_only = c.http_only,
        .secure = c.secure,
        .same_site = std::string(webview::to_string(c.same_site)),
        .is_session = c.is_session,
    };
}

/// @brief Throws std::invalid_argument when same_site is unknown or the cookie is invalid.
[[nodiscard]] inline webview::Cookie to_cookie(const CookieData& d) {
    const auto same_site = webview::parse_cookie_same_site(d.same_site);
    if (!same_site.has_value()) {
        throw std::invalid_argument("Unsupported same_site: " + d.same_site);
    }
    webview::Cookie c{
        .name = d.name,
        .value = d.value,
        .domain = d.domain,
        .path = d.path,
        .expires = d.expires,
        .http_only = d.http_only,
        .secure = d.secure,
        .same_site = *same_site,
    };
    if (const auto problem = webview::validate_cookie(c)) {
        throw std::invalid_argument(*problem);
    }
    return c;
}

/// @brief JS controller "WebView": print, PDF, capture, find, cookies and profile data of one WebView.
///
/// Bound to the WebView it is constructed with (create one per WebView/Router, as with SystemController).
/// Methods run on the router's pool thread, marshal every COM call to the UI thread and wait there for the
/// asynchronous completion (bounded by timeout), so they must never be called on the UI thread. Destroy
/// the controller on the UI thread, before the WebView.
class WebViewController {
public:
    BOSON_CONTROLLER("WebView");

    /// Event name suggested for forwarding on_find_state through Bridge::emit_to.
    static constexpr std::string_view kFindEvent = "webview:find";

    WebViewController(
        core::WebView& webview,
        exec::UiMarshal& marshal,
        std::chrono::milliseconds timeout = std::chrono::seconds{60})
        : m_webview(webview), m_marshal(marshal), m_timeout(timeout) {}

    WebViewController(const WebViewController&) = delete;
    WebViewController& operator=(const WebViewController&) = delete;

    /// Called on the UI thread whenever the find match count or active match changes, e.g.
    /// ctl.on_find_state = [&](const FindStatus& s) { bridge.emit_to(wv.id(), WebViewController::kFindEvent, s); };
    std::function<void(const FindStatus&)> on_find_state;

    /// Event name suggested for forwarding on_cdp_event through Bridge::emit_to.
    static constexpr std::string_view kCdpEvent = "cdp:event";
    /// Called on the UI thread for every CDP event subscribed through cdp_subscribe, e.g.
    /// ctl.on_cdp_event = [&](const CdpEventPayload& e) { bridge.emit_to(wv.id(), WebViewController::kCdpEvent, e); };
    std::function<void(const CdpEventPayload&)> on_cdp_event;

    // ---- Print -----------------------------------------------------------------------------------------
    [[nodiscard]] OkResponse show_print_ui(PrintUiRequest req) const {
        const auto kind = webview::parse_print_dialog_kind(req.kind);
        if (!kind.has_value()) {
            throw std::invalid_argument("Unsupported print dialog kind: " + req.kind);
        }
        return OkResponse{.ok = m_marshal.invoke_sync([this, k = *kind]() { return webview::show_print_ui(m_webview, k); })};
    }

    [[nodiscard]] PrintResult print(PrintOptions req) const {
        const webview::PrintSettings settings = to_print_settings(req);
        const auto status = await_ui<webview::PrintStatus>(
            [this, settings](std::function<void(HRESULT, webview::PrintStatus)> done) {
                return webview::print(m_webview, settings, std::move(done));
            },
            "WebView.print");
        return PrintResult{.ok = status == webview::PrintStatus::Succeeded, .status = std::string(webview::to_string(status))};
    }

    [[nodiscard]] PdfResult print_to_pdf(PrintToPdfRequest req) const {
        if (req.path.empty()) {
            throw std::invalid_argument("path must not be empty");
        }
        const webview::PrintSettings settings = to_print_settings(req.settings);
        const std::wstring path = core::utf8_to_wide(req.path);
        const bool ok = await_ui<bool>(
            [this, settings, path](std::function<void(HRESULT, bool)> done) {
                return webview::print_to_pdf(m_webview, path, settings, std::move(done));
            },
            "WebView.print_to_pdf");
        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        std::uint64_t size = 0;
        if (ok && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs) != FALSE) {
            size = (static_cast<std::uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
        }
        return PdfResult{.ok = ok, .path = std::move(req.path), .data = {}, .size = size};
    }

    [[nodiscard]] PdfResult print_to_pdf_data(PrintOptions req) const {
        const webview::PrintSettings settings = to_print_settings(req);
        const auto bytes = await_ui<std::vector<std::byte>>(
            [this, settings](std::function<void(HRESULT, std::vector<std::byte>)> done) {
                return webview::print_to_pdf_stream(m_webview, settings, std::move(done));
            },
            "WebView.print_to_pdf_data");
        return PdfResult{.ok = !bytes.empty(), .path = {}, .data = webview::encode_base64(bytes), .size = bytes.size()};
    }

    // ---- Capture ---------------------------------------------------------------------------------------
    [[nodiscard]] CaptureResult capture(CaptureRequest req) const {
        const auto format = webview::parse_image_format(req.format);
        if (!format.has_value()) {
            throw std::invalid_argument("Unsupported image format: " + req.format);
        }
        CaptureResult out{.mime_type = std::string(webview::mime_type(*format)), .data = {}, .path = {}, .size = 0};
        if (req.path.has_value() && !req.path->empty()) {
            const std::wstring path = core::utf8_to_wide(*req.path);
            await_ui<bool>(
                [this, f = *format, path](std::function<void(HRESULT, bool)> done) {
                    return webview::capture_preview_to_file(m_webview, f, path, [done](HRESULT hr) { done(hr, SUCCEEDED(hr)); });
                },
                "WebView.capture");
            WIN32_FILE_ATTRIBUTE_DATA attrs{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs) != FALSE) {
                out.size = (static_cast<std::uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
            }
            out.path = *req.path;
            return out;
        }
        const auto bytes = await_ui<std::vector<std::byte>>(
            [this, f = *format](std::function<void(HRESULT, std::vector<std::byte>)> done) {
                return webview::capture_preview(m_webview, f, std::move(done));
            },
            "WebView.capture");
        out.size = bytes.size();
        out.data = webview::encode_base64(bytes);
        return out;
    }

    // ---- Find ------------------------------------------------------------------------------------------
    [[nodiscard]] FindStatus find_start(FindRequest req) {
        if (req.term.empty()) {
            throw std::invalid_argument("term must not be empty");
        }
        const webview::FindOptions options{
            .term = std::move(req.term),
            .case_sensitive = req.case_sensitive,
            .whole_word = req.whole_word,
            .highlight_all = req.highlight_all,
            .suppress_default_dialog = req.suppress_default_dialog,
        };
        const auto state = await_ui<webview::FindState>(
            [this, options](std::function<void(HRESULT, webview::FindState)> done) {
                webview::FindSession* session = find_session();
                return session != nullptr && session->start(options, std::move(done));
            },
            "WebView.find_start");
        return FindStatus{.match_count = state.match_count, .active_match_index = state.active_match_index, .active = true};
    }

    [[nodiscard]] FindStatus find_next() {
        return find_step([](webview::FindSession& s) { return s.next(); });
    }

    [[nodiscard]] FindStatus find_previous() {
        return find_step([](webview::FindSession& s) { return s.previous(); });
    }

    [[nodiscard]] FindStatus find_stop() {
        return find_step([](webview::FindSession& s) { return s.stop(); });
    }

    [[nodiscard]] FindStatus find_status() {
        return find_step([](webview::FindSession&) { return true; });
    }

    // ---- Cookies ---------------------------------------------------------------------------------------
    [[nodiscard]] std::vector<CookieData> get_cookies(CookieQuery req) const {
        const auto cookies = await_ui<std::vector<webview::Cookie>>(
            [this, uri = std::move(req.uri)](std::function<void(HRESULT, std::vector<webview::Cookie>)> done) {
                return webview::get_cookies(m_webview, uri, std::move(done));
            },
            "WebView.get_cookies");
        std::vector<CookieData> out;
        out.reserve(cookies.size());
        for (const auto& c : cookies) {
            out.push_back(to_cookie_data(c));
        }
        return out;
    }

    [[nodiscard]] OkResponse set_cookie(CookieData req) const {
        const webview::Cookie cookie = to_cookie(req);
        return OkResponse{.ok = m_marshal.invoke_sync([this, &cookie]() { return webview::set_cookie(m_webview, cookie); })};
    }

    [[nodiscard]] OkResponse delete_cookies(DeleteCookiesRequest req) const {
        if (req.name.empty()) {
            throw std::invalid_argument("name must not be empty");
        }
        if (!req.uri.has_value() && !req.domain.has_value()) {
            throw std::invalid_argument("delete_cookies needs either uri or domain (+ path)");
        }
        return OkResponse{.ok = m_marshal.invoke_sync([this, &req]() {
            if (req.uri.has_value()) {
                return webview::delete_cookies(m_webview, req.name, *req.uri);
            }
            return webview::delete_cookies(m_webview, req.name, *req.domain, req.path.value_or("/"));
        })};
    }

    [[nodiscard]] OkResponse delete_all_cookies() const {
        return OkResponse{.ok = m_marshal.invoke_sync([this]() { return webview::delete_all_cookies(m_webview); })};
    }

    // ---- Profile / browsing data ------------------------------------------------------------------------
    [[nodiscard]] OkResponse clear_data(ClearDataRequest req) const {
        const auto kinds = webview::parse_browsing_data_kinds(req.kinds);
        if (!kinds.has_value()) {
            throw std::invalid_argument("Unknown browsing data kind in kinds");
        }
        const bool ok = await_ui<bool>(
            [this, k = *kinds, req](std::function<void(HRESULT, bool)> done) {
                return webview::clear_browsing_data(m_webview, k, req.start_time, req.end_time, [done](HRESULT hr) { done(hr, SUCCEEDED(hr)); });
            },
            "WebView.clear_data");
        return OkResponse{.ok = ok};
    }

    [[nodiscard]] ProfileData get_profile() const {
        const auto info = m_marshal.invoke_sync([this]() { return webview::profile_info(m_webview); });
        if (!info.has_value()) {
            throw std::runtime_error("WebView profile is unavailable");
        }
        ProfileData out{
            .name = info->name,
            .in_private = info->in_private,
            .path = info->path,
            .default_download_folder = info->default_download_folder,
            .color_scheme = std::string(webview::to_string(info->color_scheme)),
            .tracking_prevention = std::nullopt,
        };
        if (info->tracking_prevention.has_value()) {
            out.tracking_prevention = std::string(webview::to_string(*info->tracking_prevention));
        }
        return out;
    }

    [[nodiscard]] OkResponse set_color_scheme(ColorSchemeRequest req) const {
        const auto scheme = webview::parse_color_scheme(req.scheme);
        if (!scheme.has_value()) {
            throw std::invalid_argument("Unsupported color scheme: " + req.scheme);
        }
        return OkResponse{.ok = m_marshal.invoke_sync([this, s = *scheme]() { return webview::set_preferred_color_scheme(m_webview, s); })};
    }

    // ---- DRM capability probe ---------------------------------------------------------------------------
    /// Runs webview::probe_drm in the calling WebView's top document. Omitted fields keep the defaults
    /// (PlayReady SL3000/SL2000, Widevine HW/SW, Clear Key; avc1/hev1/vp09/av01; cenc/cbcs).
    [[nodiscard]] webview::DrmProbeResult probe_drm(DrmProbeRequest request) const {
        webview::DrmProbeOptions req = to_drm_probe_options(std::move(request));
        return await_ui<webview::DrmProbeResult>(
            [this, req = std::move(req)](std::function<void(HRESULT, webview::DrmProbeResult)> done) {
                return webview::probe_drm(m_webview, req, std::move(done));
            },
            "WebView.probe_drm");
    }

    // ---- Chrome DevTools Protocol ------------------------------------------------------------------------
    /// Runs a CDP command on this WebView (CallDevToolsProtocolMethod[ForSession]); resolves with its result.
    [[nodiscard]] CdpCommandResult cdp_send(CdpCommand req) {
        if (req.method.empty()) {
            throw std::invalid_argument("method must not be empty");
        }
        struct Outcome {
            bool ok = false;
            std::string json;
        };
        const std::string params = req.params.str.empty() ? std::string("{}") : req.params.str;
        const auto outcome = await_ui<Outcome>(
            [this, method = req.method, params, session = req.session_id](std::function<void(HRESULT, Outcome)> done) {
                return cdp().call(method, params, [done](bool ok, std::string json) { done(S_OK, Outcome{.ok = ok, .json = std::move(json)}); }, session);
            },
            "WebView.cdp_send");
        if (!outcome.ok) {
            struct ErrorBody {
                std::int32_t code = 0;
                std::string message;
            } err;
            (void)glz::read<glz::opts{.error_on_unknown_keys = false}>(err, outcome.json);
            throw std::runtime_error(err.message.empty() ? outcome.json : err.message);
        }
        return CdpCommandResult{.result = glz::raw_json{outcome.json}};
    }

    /// Adds (or removes) CDP event subscriptions; events reach on_cdp_event ("cdp:event").
    [[nodiscard]] OkResponse cdp_subscribe(CdpSubscription req) {
        return OkResponse{.ok = m_marshal.invoke_sync([this, &req]() {
            webview::Cdp& c = cdp();
            for (const auto& event : req.events) {
                const auto it = std::ranges::find(m_cdp_subs, event, &std::pair<std::string, webview::Cdp::SubscriptionId>::first);
                if (req.subscribe && it == m_cdp_subs.end()) {
                    const auto id = c.subscribe(event, [this](std::string_view method, std::string_view params, std::string_view session) {
                        if (on_cdp_event) {
                            on_cdp_event(CdpEventPayload{.method = std::string(method), .params = glz::raw_json{std::string(params)}, .session_id = std::string(session)});
                        }
                    });
                    if (id != 0) {
                        m_cdp_subs.emplace_back(event, id);
                    }
                } else if (!req.subscribe && it != m_cdp_subs.end()) {
                    c.unsubscribe(it->second);
                    m_cdp_subs.erase(it);
                }
            }
            return true;
        })};
    }

private:
    /// UI thread only. Created on first use.
    webview::Cdp& cdp() {
        if (!m_cdp) {
            m_cdp = std::make_unique<webview::Cdp>(m_webview);
        }
        return *m_cdp;
    }

    template <typename T>
    struct Pending {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        HRESULT hr = E_PENDING;
        T value{};

        void complete(HRESULT result, T v) {
            {
                std::scoped_lock lock(mutex);
                if (done) {
                    return;
                }
                done = true;
                hr = result;
                value = std::move(v);
            }
            cv.notify_all();
        }
    };

    /// Starts an async UI-thread operation and blocks this (pool) thread until it completes. start returns
    /// false when the operation could not be started. Throws std::runtime_error on failure or timeout.
    template <typename T, typename Start>
    T await_ui(Start start, std::string_view what) const {
        auto pending = std::make_shared<Pending<T>>();
        m_marshal.post_task([pending, start = std::move(start)]() mutable {
            bool started = false;
            try {
                started = start([pending](HRESULT hr, T value) { pending->complete(hr, std::move(value)); });
            } catch (...) {
                started = false;
            }
            if (!started) {
                pending->complete(E_FAIL, T{});
            }
        });
        std::unique_lock lock(pending->mutex);
        if (!pending->cv.wait_for(lock, m_timeout, [&pending]() { return pending->done; })) {
            throw std::runtime_error(std::string(what) + " timed out");
        }
        if (FAILED(pending->hr)) {
            char code[16]{};
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(pending->hr));
            throw std::runtime_error(std::string(what) + " failed (HRESULT " + code + ")");
        }
        return std::move(pending->value);
    }

    /// UI thread only. Lazily binds the FindSession to the WebView's current ICoreWebView2.
    webview::FindSession* find_session() {
        ICoreWebView2* current = m_webview.core_webview();
        if (current == nullptr) {
            m_find.reset();
            m_find_owner.Reset();
            return nullptr;
        }
        if (!m_find || m_find_owner.Get() != current) {
            m_find = std::make_unique<webview::FindSession>(m_webview);
            m_find_owner = current;
            m_find->set_state_callback([this](const webview::FindState& s) {
                if (on_find_state) {
                    on_find_state(FindStatus{.match_count = s.match_count, .active_match_index = s.active_match_index, .active = m_find && m_find->is_active()});
                }
            });
        }
        return m_find->is_supported() ? m_find.get() : nullptr;
    }

    template <typename Fn>
    FindStatus find_step(Fn fn) {
        return m_marshal.invoke_sync([this, &fn]() {
            webview::FindSession* session = find_session();
            if (session == nullptr) {
                throw std::runtime_error("Find is not supported by this WebView2 runtime");
            }
            fn(*session);
            const auto s = session->state();
            return FindStatus{.match_count = s.match_count, .active_match_index = s.active_match_index, .active = session->is_active()};
        });
    }

    core::WebView& m_webview;
    exec::UiMarshal& m_marshal;
    std::chrono::milliseconds m_timeout;
    std::unique_ptr<webview::FindSession> m_find;   // UI thread only
    std::unique_ptr<webview::Cdp> m_cdp;            // UI thread only
    std::vector<std::pair<std::string, webview::Cdp::SubscriptionId>> m_cdp_subs; // UI thread only
    Microsoft::WRL::ComPtr<ICoreWebView2> m_find_owner; // the ICoreWebView2 m_find is bound to
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::WebViewController> {
    using T = boson::ipc::WebViewController;
    static constexpr auto value = glz::object(
        "show_print_ui", &T::show_print_ui,
        "print", &T::print,
        "print_to_pdf", &T::print_to_pdf,
        "print_to_pdf_data", &T::print_to_pdf_data,
        "capture", &T::capture,
        "find_start", &T::find_start,
        "find_next", &T::find_next,
        "find_previous", &T::find_previous,
        "find_stop", &T::find_stop,
        "find_status", &T::find_status,
        "get_cookies", &T::get_cookies,
        "set_cookie", &T::set_cookie,
        "delete_cookies", &T::delete_cookies,
        "delete_all_cookies", &T::delete_all_cookies,
        "clear_data", &T::clear_data,
        "get_profile", &T::get_profile,
        "set_color_scheme", &T::set_color_scheme,
        "probe_drm", &T::probe_drm,
        "cdp_send", &T::cdp_send,
        "cdp_subscribe", &T::cdp_subscribe);
};
