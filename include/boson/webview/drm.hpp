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

/// @brief One navigator.requestMediaKeySystemAccess query: a key system plus the robustness asked for.
struct DrmKeySystemQuery {
    std::string id;               ///< Stable name for the result, e.g. "playready_sl3000".
    std::string key_system;       ///< e.g. "com.microsoft.playready.recommendation.3000".
    std::string robustness;       ///< Video robustness, e.g. "3000", "HW_SECURE_ALL"; empty = any.
    std::string audio_robustness; ///< Audio robustness ("SW_SECURE_CRYPTO" for Widevine); empty = any.
    /// true when a "supported" answer means the CDM promised a hardware (TEE / protected media path) pipeline.
    bool hardware = false;
};

/// @brief A codec probed for every key system, e.g. {"avc1", "video/mp4; codecs=\"avc1.640028\""}.
struct DrmCodec {
    std::string name;
    std::string content_type;
};

/// @brief The default query lists (see DrmProbeOptions).
[[nodiscard]] std::vector<DrmKeySystemQuery> default_drm_key_systems();
[[nodiscard]] std::vector<DrmCodec> default_drm_video_codecs();
[[nodiscard]] std::vector<DrmCodec> default_drm_audio_codecs();

/// @brief What probe_drm asks. Defaults: PlayReady SL3000 / SL2000, Widevine HW_SECURE_ALL / SW_SECURE_DECODE
/// and Clear Key, each with avc1, hev1, vp09 and av01 video and mp4a, ec-3 and opus audio, cenc and cbcs.
struct DrmProbeOptions {
    std::vector<DrmKeySystemQuery> key_systems = default_drm_key_systems();
    std::vector<DrmCodec> video_codecs = default_drm_video_codecs();
    /// Probed with a separate request after a video match, so an audio robustness mismatch cannot hide a
    /// supported video pipeline. Empty = skip the audio request.
    std::vector<DrmCodec> audio_codecs = default_drm_audio_codecs();
    /// EME encryptionScheme values asked for each codec; empty = do not set encryptionScheme.
    std::vector<std::string> encryption_schemes = {"cenc", "cbcs"};
    /// Also call MediaKeySystemAccess.createMediaKeys(): this loads and initializes the CDM, which is what
    /// fails when hardware DRM is advertised but broken (driver, HDCP or MediaFoundation problems).
    bool create_media_keys = true;
    /// MediaKeys.getStatusForPolicy({minHdcpVersion: "2.2"}) where the CDM implements it.
    bool check_hdcp = true;
    /// Per requestMediaKeySystemAccess / createMediaKeys / getStatusForPolicy call.
    std::uint32_t timeout_ms = 10000;
};


/// @brief One capability the CDM accepted (an entry of MediaKeySystemConfiguration.videoCapabilities).
struct DrmCapability {
    std::string codec; ///< DrmCodec::name of the matching query entry.
    std::string content_type;
    std::string encryption_scheme;
    std::string robustness;
};

struct DrmKeySystemResult {
    std::string id;
    std::string key_system;
    std::string robustness;
    bool hardware = false;
    /// requestMediaKeySystemAccess resolved for at least one video capability.
    bool supported = false;
    std::vector<DrmCapability> video;
    std::vector<DrmCapability> audio;
    std::vector<std::string> init_data_types;
    std::vector<std::string> session_types;
    std::string persistent_state;
    std::string distinctive_identifier;
    /// null when not attempted (create_media_keys = false or unsupported).
    std::optional<bool> media_keys_created;
    /// MediaKeyStatus for minHdcpVersion 2.2 ("usable", "output-restricted", ...); empty when not checked,
    /// "unsupported" when the CDM has no getStatusForPolicy, "error: ..." when it rejected.
    std::string hdcp_2_2;
    std::string error; ///< DOMException name and message of the failed step; empty on success.
    double elapsed_ms = 0.0;
};

enum class DrmProtection {
    Hardware,     ///< At least one hardware query is supported (and its CDM initialized when checked).
    Software,     ///< Only software PlayReady / Widevine.
    ClearKeyOnly, ///< Only org.w3.clearkey: EME works, but no commercial DRM is available.
    None,         ///< No key system at all (or EME unavailable).
};

[[nodiscard]] std::string_view to_string(DrmProtection protection) noexcept;

struct DrmProbeResult {
    bool secure_context = false;
    bool eme_available = false;
    std::string origin;
    std::string user_agent;
    std::vector<DrmKeySystemResult> key_systems;
    /// to_string(summarize(*this)), filled in by probe_drm / parse_drm_probe_result.
    std::string protection;
    /// id of the first usable hardware result, else the first usable result; empty when none.
    std::string best;
    std::string error; ///< Set when the probe could not run (no EME, not a secure context, script error).
};

/// @brief Classifies a result. A query counts as usable when it is supported and createMediaKeys did not fail.
[[nodiscard]] DrmProtection summarize(const DrmProbeResult& result) noexcept;

/// @brief The JavaScript expression probe_drm evaluates: an async function that resolves to the JSON text of a
/// DrmProbeResult. Exposed for tests and for apps that want to run it themselves.
[[nodiscard]] std::string build_drm_probe_script(const DrmProbeOptions& options);

/// @brief Parses the JSON produced by the probe script and fills protection / best. nullopt on malformed input.
[[nodiscard]] std::optional<DrmProbeResult> parse_drm_probe_result(std::string_view json);

/// @brief Runs the EME capability probe in the WebView's top document (DevTools Runtime.evaluate with
/// awaitPromise, so it works on third-party pages, ignores the page CSP and needs no web messages).
///
/// UI thread; callback on the UI thread. EME needs a secure context: https:, boson://, http://localhost or a
/// top-level about:blank. On a page without EME the callback gets S_OK with eme_available = false and error set.
/// The callback gets a failure HRESULT when the evaluation itself failed (navigation in progress, renderer gone).
/// Returns false (callback not invoked) when the WebView is not ready.
bool probe_drm(core::WebView& webview, std::function<void(HRESULT, DrmProbeResult)> callback);
bool probe_drm(core::WebView& webview, const DrmProbeOptions& options, std::function<void(HRESULT, DrmProbeResult)> callback);
[[nodiscard]] exec::CallbackSender<DrmProbeResult> probe_drm(core::WebView& webview, DrmProbeOptions options = {});

} // namespace boson::webview
