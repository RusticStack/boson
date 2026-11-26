#include <boson/webview/profile.hpp>

#include "detail.hpp"

#include <array>
#include <cwctype>
#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct KindName {
    std::string_view name;
    BrowsingDataKind kind;
};

constexpr std::array<KindName, 16> kKindNames{{
    {"file_systems", BrowsingDataKind::FileSystems},
    {"indexed_db", BrowsingDataKind::IndexedDb},
    {"local_storage", BrowsingDataKind::LocalStorage},
    {"web_sql", BrowsingDataKind::WebSql},
    {"cache_storage", BrowsingDataKind::CacheStorage},
    {"all_dom_storage", BrowsingDataKind::AllDomStorage},
    {"cookies", BrowsingDataKind::Cookies},
    {"all_site", BrowsingDataKind::AllSite},
    {"disk_cache", BrowsingDataKind::DiskCache},
    {"download_history", BrowsingDataKind::DownloadHistory},
    {"general_autofill", BrowsingDataKind::GeneralAutofill},
    {"password_autosave", BrowsingDataKind::PasswordAutosave},
    {"browsing_history", BrowsingDataKind::BrowsingHistory},
    {"settings", BrowsingDataKind::Settings},
    {"all_profile", BrowsingDataKind::AllProfile},
    {"service_workers", BrowsingDataKind::ServiceWorkers},
}};

COREWEBVIEW2_PREFERRED_COLOR_SCHEME to_native(ColorScheme v) noexcept {
    switch (v) {
    case ColorScheme::Light: return COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT;
    case ColorScheme::Dark: return COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK;
    case ColorScheme::Auto: break;
    }
    return COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
}

ColorScheme from_native(COREWEBVIEW2_PREFERRED_COLOR_SCHEME v) noexcept {
    switch (v) {
    case COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT: return ColorScheme::Light;
    case COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK: return ColorScheme::Dark;
    case COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO: break;
    }
    return ColorScheme::Auto;
}

COREWEBVIEW2_TRACKING_PREVENTION_LEVEL to_native(TrackingPreventionLevel v) noexcept {
    switch (v) {
    case TrackingPreventionLevel::None: return COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_NONE;
    case TrackingPreventionLevel::Basic: return COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_BASIC;
    case TrackingPreventionLevel::Strict: return COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_STRICT;
    case TrackingPreventionLevel::Balanced: break;
    }
    return COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_BALANCED;
}

TrackingPreventionLevel from_native(COREWEBVIEW2_TRACKING_PREVENTION_LEVEL v) noexcept {
    switch (v) {
    case COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_NONE: return TrackingPreventionLevel::None;
    case COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_BASIC: return TrackingPreventionLevel::Basic;
    case COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_STRICT: return TrackingPreventionLevel::Strict;
    case COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_BALANCED: break;
    }
    return TrackingPreventionLevel::Balanced;
}

} // namespace

std::optional<BrowsingDataKind> parse_browsing_data_kind(std::string_view name) noexcept {
    for (const auto& entry : kKindNames) {
        if (entry.name == name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

std::optional<BrowsingDataKind> parse_browsing_data_kinds(const std::vector<std::string>& names) noexcept {
    BrowsingDataKind out = BrowsingDataKind::None;
    for (const auto& name : names) {
        const auto kind = parse_browsing_data_kind(name);
        if (!kind.has_value()) {
            return std::nullopt;
        }
        out = out | *kind;
    }
    return out;
}

std::string_view to_string(ColorScheme scheme) noexcept {
    switch (scheme) {
    case ColorScheme::Light: return "light";
    case ColorScheme::Dark: return "dark";
    case ColorScheme::Auto: break;
    }
    return "auto";
}

std::string_view to_string(TrackingPreventionLevel level) noexcept {
    switch (level) {
    case TrackingPreventionLevel::None: return "none";
    case TrackingPreventionLevel::Basic: return "basic";
    case TrackingPreventionLevel::Strict: return "strict";
    case TrackingPreventionLevel::Balanced: break;
    }
    return "balanced";
}

std::optional<ColorScheme> parse_color_scheme(std::string_view name) noexcept {
    if (name == "auto") return ColorScheme::Auto;
    if (name == "light") return ColorScheme::Light;
    if (name == "dark") return ColorScheme::Dark;
    return std::nullopt;
}

std::optional<TrackingPreventionLevel> parse_tracking_prevention_level(std::string_view name) noexcept {
    if (name == "none") return TrackingPreventionLevel::None;
    if (name == "basic") return TrackingPreventionLevel::Basic;
    if (name == "balanced") return TrackingPreventionLevel::Balanced;
    if (name == "strict") return TrackingPreventionLevel::Strict;
    return std::nullopt;
}

bool is_valid_profile_name(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > 64 || name.back() == L' ' || name.back() == L'.') {
        return false;
    }
    constexpr std::wstring_view kAllowed = L" #@$()+-_~.";
    for (const wchar_t ch : name) {
        const bool alnum = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
        if (!alnum && kAllowed.find(ch) == std::wstring_view::npos) {
            return false;
        }
    }
    return true;
}

bool wants_controller_options(const core::WebViewConfig& config) noexcept {
    return !config.profile_name.empty() || config.in_private || !config.script_locale.empty();
}

ComPtr<ICoreWebView2ControllerOptions> create_controller_options(
    ICoreWebView2Environment* env,
    const core::WebViewConfig& config) {
    if (env == nullptr || !wants_controller_options(config)) {
        return nullptr;
    }
    if (!config.profile_name.empty() && !is_valid_profile_name(config.profile_name)) {
        core::hr_check(E_INVALIDARG, "WebViewConfig::profile_name is not a valid WebView2 profile name");
        return nullptr;
    }
    ComPtr<ICoreWebView2Environment10> env10;
    if (!BOSON_HR_CHECK(env->QueryInterface(IID_PPV_ARGS(&env10)))) {
        return nullptr;
    }
    ComPtr<ICoreWebView2ControllerOptions> options;
    if (!BOSON_HR_CHECK(env10->CreateCoreWebView2ControllerOptions(&options)) || !options) {
        return nullptr;
    }
    if (!config.profile_name.empty() && !BOSON_HR_CHECK(options->put_ProfileName(config.profile_name.c_str()))) {
        return nullptr;
    }
    if (config.in_private && !BOSON_HR_CHECK(options->put_IsInPrivateModeEnabled(TRUE))) {
        return nullptr;
    }
    if (!config.script_locale.empty()) {
        ComPtr<ICoreWebView2ControllerOptions2> options2;
        if (!BOSON_HR_CHECK(options.As(&options2)) || !BOSON_HR_CHECK(options2->put_ScriptLocale(config.script_locale.c_str()))) {
            return nullptr;
        }
    }
    return options;
}

ComPtr<ICoreWebView2Profile> profile_of(const core::WebView& webview) noexcept {
    ComPtr<ICoreWebView2Profile> profile;
    if (const auto webview13 = detail::require<ICoreWebView2_13>(webview, "ICoreWebView2_13 (Profile)")) {
        if (!BOSON_HR_CHECK(webview13->get_Profile(&profile))) {
            profile.Reset();
        }
    }
    return profile;
}

std::optional<ProfileInfo> profile_info(const core::WebView& webview) {
    const auto profile = profile_of(webview);
    if (!profile) {
        return std::nullopt;
    }
    ProfileInfo info;
    wchar_t* raw = nullptr;
    HRESULT hr = profile->get_ProfileName(&raw);
    info.name = detail::take_utf8(hr, raw, "Profile::get_ProfileName");
    BOOL in_private = FALSE;
    if (BOSON_HR_CHECK(profile->get_IsInPrivateModeEnabled(&in_private))) {
        info.in_private = in_private != FALSE;
    }
    raw = nullptr;
    hr = profile->get_ProfilePath(&raw);
    info.path = detail::take_utf8(hr, raw, "Profile::get_ProfilePath");
    raw = nullptr;
    hr = profile->get_DefaultDownloadFolderPath(&raw);
    info.default_download_folder = detail::take_utf8(hr, raw, "Profile::get_DefaultDownloadFolderPath");
    COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
    if (BOSON_HR_CHECK(profile->get_PreferredColorScheme(&scheme))) {
        info.color_scheme = from_native(scheme);
    }
    ComPtr<ICoreWebView2Profile3> profile3;
    if (SUCCEEDED(profile.As(&profile3)) && profile3) {
        COREWEBVIEW2_TRACKING_PREVENTION_LEVEL level = COREWEBVIEW2_TRACKING_PREVENTION_LEVEL_BALANCED;
        if (SUCCEEDED(profile3->get_PreferredTrackingPreventionLevel(&level))) {
            info.tracking_prevention = from_native(level);
        }
    }
    return info;
}

bool set_preferred_color_scheme(core::WebView& webview, ColorScheme scheme) noexcept {
    const auto profile = profile_of(webview);
    return profile && BOSON_HR_CHECK(profile->put_PreferredColorScheme(to_native(scheme)));
}

bool set_default_download_folder(core::WebView& webview, std::wstring_view folder) {
    const auto profile = profile_of(webview);
    if (!profile || folder.empty()) {
        return false;
    }
    const std::wstring path(folder);
    return BOSON_HR_CHECK(profile->put_DefaultDownloadFolderPath(path.c_str()));
}

bool set_tracking_prevention_level(core::WebView& webview, TrackingPreventionLevel level) noexcept {
    const auto profile = profile_of(webview);
    ComPtr<ICoreWebView2Profile3> profile3;
    return profile && BOSON_HR_CHECK(profile.As(&profile3)) &&
           BOSON_HR_CHECK(profile3->put_PreferredTrackingPreventionLevel(to_native(level)));
}

bool clear_browsing_data(
    core::WebView& webview,
    BrowsingDataKind kinds,
    std::optional<double> start_time,
    std::optional<double> end_time,
    std::function<void(HRESULT)> callback) {
    const auto profile = profile_of(webview);
    ComPtr<ICoreWebView2Profile2> profile2;
    if (!profile || !BOSON_HR_CHECK(profile.As(&profile2))) {
        return false;
    }
    const auto handler = Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
        [callback = std::move(callback)](HRESULT hr) -> HRESULT {
            if (callback) {
                callback(hr);
            }
            return S_OK;
        });
    if (kinds == BrowsingDataKind::None) {
        return BOSON_HR_CHECK(profile2->ClearBrowsingDataAll(handler.Get()));
    }
    const auto native_kinds = static_cast<COREWEBVIEW2_BROWSING_DATA_KINDS>(static_cast<std::uint32_t>(kinds));
    if (!start_time.has_value() && !end_time.has_value()) {
        return BOSON_HR_CHECK(profile2->ClearBrowsingData(native_kinds, handler.Get()));
    }
    const double start = start_time.value_or(0.0);
    double end = 0.0;
    if (end_time.has_value()) {
        end = *end_time;
    } else {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const std::uint64_t ticks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        constexpr std::uint64_t kEpochDelta = 116444736000000000ULL; // 1601-01-01 -> 1970-01-01 in 100 ns
        end = static_cast<double>(ticks - kEpochDelta) / 1.0e7 + 1.0;
    }
    if (end < start) {
        core::hr_check(E_INVALIDARG, "clear_browsing_data: end_time is before start_time");
        return false;
    }
    return BOSON_HR_CHECK(profile2->ClearBrowsingDataInTimeRange(native_kinds, start, end, handler.Get()));
}

exec::CallbackSender<bool> clear_browsing_data(
    core::WebView& webview,
    BrowsingDataKind kinds,
    std::optional<double> start_time,
    std::optional<double> end_time) {
    return exec::CallbackSender<bool>(
        [&webview, kinds, start_time, end_time](exec::Completion<bool> done) {
            if (!clear_browsing_data(webview, kinds, start_time, end_time, [done](HRESULT hr) { done(hr, SUCCEEDED(hr)); })) {
                done(E_FAIL, false);
            }
        },
        "ICoreWebView2Profile2::ClearBrowsingData");
}

} // namespace boson::webview
