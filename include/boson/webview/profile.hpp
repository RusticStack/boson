#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/callback_sender.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace boson::webview {

/// @brief COREWEBVIEW2_BROWSING_DATA_KINDS, combinable with |.
enum class BrowsingDataKind : std::uint32_t {
    None = 0,
    FileSystems = 0x1,
    IndexedDb = 0x2,
    LocalStorage = 0x4,
    WebSql = 0x8,
    CacheStorage = 0x10,
    AllDomStorage = 0x20,
    Cookies = 0x40,
    AllSite = 0x80,
    DiskCache = 0x100,
    DownloadHistory = 0x200,
    GeneralAutofill = 0x400,
    PasswordAutosave = 0x800,
    BrowsingHistory = 0x1000,
    Settings = 0x2000,
    AllProfile = 0x4000,
    ServiceWorkers = 0x8000,
};

[[nodiscard]] constexpr BrowsingDataKind operator|(BrowsingDataKind a, BrowsingDataKind b) noexcept {
    return static_cast<BrowsingDataKind>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/// @brief "cookies", "disk_cache", "local_storage", ... (snake_case of the enumerators). nullopt if unknown.
[[nodiscard]] std::optional<BrowsingDataKind> parse_browsing_data_kind(std::string_view name) noexcept;
/// @brief Combines names; returns nullopt naming nothing when any name is unknown.
[[nodiscard]] std::optional<BrowsingDataKind> parse_browsing_data_kinds(const std::vector<std::string>& names) noexcept;

enum class ColorScheme {
    Auto,
    Light,
    Dark,
};

enum class TrackingPreventionLevel {
    None,
    Basic,
    Balanced,
    Strict,
};

[[nodiscard]] std::string_view to_string(ColorScheme scheme) noexcept;
[[nodiscard]] std::string_view to_string(TrackingPreventionLevel level) noexcept;
[[nodiscard]] std::optional<ColorScheme> parse_color_scheme(std::string_view name) noexcept;
[[nodiscard]] std::optional<TrackingPreventionLevel> parse_tracking_prevention_level(std::string_view name) noexcept;

/// @brief WebView2 profile name rules: 1-64 characters from letters, digits and " #@$()+-_~." and it must
/// not end with a space or a period.
[[nodiscard]] bool is_valid_profile_name(std::wstring_view name) noexcept;

/// @brief ICoreWebView2ControllerOptions for config.profile_name / in_private / script_locale, or null when
/// all of them are defaults (then the plain CreateCoreWebView2Controller path is used). Also null, with a
/// log line, when the runtime lacks ICoreWebView2Environment10 or the profile name is invalid.
[[nodiscard]] Microsoft::WRL::ComPtr<ICoreWebView2ControllerOptions> create_controller_options(
    ICoreWebView2Environment* env,
    const core::WebViewConfig& config);

/// @brief true when config asks for controller options (named profile, InPrivate or a script locale).
[[nodiscard]] bool wants_controller_options(const core::WebViewConfig& config) noexcept;

struct ProfileInfo {
    std::string name;                 ///< Empty string for the default profile.
    bool in_private = false;
    std::string path;                 ///< Folder holding the profile's data (empty for InPrivate on some runtimes).
    std::string default_download_folder;
    ColorScheme color_scheme = ColorScheme::Auto;
    std::optional<TrackingPreventionLevel> tracking_prevention; ///< ICoreWebView2Profile3
};

/// @brief ICoreWebView2_13::get_Profile of the WebView.
[[nodiscard]] Microsoft::WRL::ComPtr<ICoreWebView2Profile> profile_of(const core::WebView& webview) noexcept;
[[nodiscard]] std::optional<ProfileInfo> profile_info(const core::WebView& webview);

/// The setters apply to every WebView sharing the profile. UI thread.
bool set_preferred_color_scheme(core::WebView& webview, ColorScheme scheme) noexcept;
bool set_default_download_folder(core::WebView& webview, std::wstring_view folder);
/// @brief Requires EnvironmentConfig::enable_tracking_prevention not set to false.
bool set_tracking_prevention_level(core::WebView& webview, TrackingPreventionLevel level) noexcept;

/// @brief ICoreWebView2Profile2::ClearBrowsingData[InTimeRange]. Times are seconds since the UNIX epoch;
/// with both unset, all data of those kinds is cleared. kinds == None clears every kind (ClearBrowsingDataAll).
bool clear_browsing_data(
    core::WebView& webview,
    BrowsingDataKind kinds,
    std::optional<double> start_time,
    std::optional<double> end_time,
    std::function<void(HRESULT)> callback);
[[nodiscard]] exec::CallbackSender<bool> clear_browsing_data(
    core::WebView& webview,
    BrowsingDataKind kinds,
    std::optional<double> start_time = std::nullopt,
    std::optional<double> end_time = std::nullopt);

} // namespace boson::webview
