#include <boson/webview/drm.hpp>

#include "detail.hpp"

#include <glaze/glaze.hpp>

#include <memory>
#include <utility>

namespace boson::webview {
namespace detail {

/// DevTools Runtime.evaluate parameters (https://chromedevtools.github.io/devtools-protocol/tot/Runtime/#method-evaluate).
struct RuntimeEvaluateParams {
    std::string expression;
    bool await_promise = true;
    bool return_by_value = true;
    bool silent = true;
};

struct RuntimeRemoteObject {
    std::string type;
    glz::raw_json value{};
    std::string description;
};

struct RuntimeExceptionDetails {
    std::string text;
};

struct RuntimeEvaluateResponse {
    RuntimeRemoteObject result;
    std::optional<RuntimeExceptionDetails> exception_details;
};

} // namespace detail
} // namespace boson::webview

template <>
struct glz::meta<boson::webview::detail::RuntimeEvaluateParams> {
    using T = boson::webview::detail::RuntimeEvaluateParams;
    static constexpr auto value = glz::object(
        "expression", &T::expression,
        "awaitPromise", &T::await_promise,
        "returnByValue", &T::return_by_value,
        "silent", &T::silent);
};

template <>
struct glz::meta<boson::webview::detail::RuntimeEvaluateResponse> {
    using T = boson::webview::detail::RuntimeEvaluateResponse;
    static constexpr auto value = glz::object(
        "result", &T::result,
        "exceptionDetails", &T::exception_details);
};

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;

constexpr glz::opts kLenient{.error_on_unknown_keys = false};

// The probe. Kept as one expression: Runtime.evaluate calls it with the options object and awaits the promise.
constexpr std::string_view kProbeFunction = R"BOSONJS((async (cfg) => {
  const out = { secure_context: !!window.isSecureContext, eme_available: false, origin: String(location.origin),
    user_agent: String(navigator.userAgent), key_systems: [], protection: '', best: '', error: '' };
  const message = (e) => (e && e.name ? e.name + ': ' : '') + (e && e.message !== undefined ? e.message : String(e));
  if (typeof navigator.requestMediaKeySystemAccess !== 'function') {
    out.error = window.isSecureContext ? 'navigator.requestMediaKeySystemAccess is unavailable'
                                       : 'EME needs a secure context (https:, boson:, http://localhost)';
    return JSON.stringify(out);
  }
  out.eme_available = true;
  const limit = (promise, what) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(what + ' timed out after ' + cfg.timeout_ms + ' ms')), cfg.timeout_ms);
    promise.then((v) => { clearTimeout(timer); resolve(v); }, (e) => { clearTimeout(timer); reject(e); });
  });
  const capabilities = (codecs, robustness) => {
    const list = [];
    for (const codec of codecs) {
      const schemes = cfg.encryption_schemes.length ? cfg.encryption_schemes : [null];
      for (const scheme of schemes) {
        const cap = { contentType: codec.content_type, robustness: robustness };
        if (scheme !== null) { cap.encryptionScheme = scheme; }
        list.push(cap);
      }
    }
    return list;
  };
  const describe = (codecs, granted) => (granted || []).map((c) => ({
    codec: (codecs.find((x) => x.content_type === c.contentType) || { name: String(c.contentType) }).name,
    content_type: String(c.contentType || ''), encryption_scheme: String(c.encryptionScheme || ''),
    robustness: String(c.robustness || '') }));
  const base = (q) => ({ label: q.id, initDataTypes: ['cenc', 'keyids', 'webm'], distinctiveIdentifier: 'optional',
    persistentState: 'optional', sessionTypes: ['temporary'] });
  for (const q of cfg.key_systems) {
    const started = performance.now();
    const r = { id: q.id, key_system: q.key_system, robustness: q.robustness, hardware: !!q.hardware, supported: false,
      video: [], audio: [], init_data_types: [], session_types: [], persistent_state: '', distinctive_identifier: '',
      media_keys_created: null, hdcp_2_2: '', error: '', elapsed_ms: 0 };
    try {
      const config = base(q);
      config.videoCapabilities = capabilities(cfg.video_codecs, q.robustness);
      const access = await limit(navigator.requestMediaKeySystemAccess(q.key_system, [config]), 'requestMediaKeySystemAccess');
      const granted = access.getConfiguration();
      r.supported = true;
      r.video = describe(cfg.video_codecs, granted.videoCapabilities);
      r.init_data_types = (granted.initDataTypes || []).map(String);
      r.session_types = (granted.sessionTypes || []).map(String);
      r.persistent_state = String(granted.persistentState || '');
      r.distinctive_identifier = String(granted.distinctiveIdentifier || '');
      if (cfg.audio_codecs.length) {
        try {
          const audio = base(q);
          audio.audioCapabilities = capabilities(cfg.audio_codecs, q.audio_robustness);
          const a = await limit(navigator.requestMediaKeySystemAccess(q.key_system, [audio]), 'requestMediaKeySystemAccess (audio)');
          r.audio = describe(cfg.audio_codecs, a.getConfiguration().audioCapabilities);
        } catch (e) { /* no audio capability for this robustness: r.audio stays empty */ }
      }
      if (cfg.create_media_keys) {
        let keys = null;
        try {
          keys = await limit(access.createMediaKeys(), 'createMediaKeys');
          r.media_keys_created = true;
        } catch (e) {
          r.media_keys_created = false;
          r.error = 'createMediaKeys: ' + message(e);
        }
        if (keys && cfg.check_hdcp) {
          if (typeof keys.getStatusForPolicy !== 'function') {
            r.hdcp_2_2 = 'unsupported';
          } else {
            try { r.hdcp_2_2 = String(await limit(keys.getStatusForPolicy({ minHdcpVersion: '2.2' }), 'getStatusForPolicy')); }
            catch (e) { r.hdcp_2_2 = 'error: ' + message(e); }
          }
        }
      }
    } catch (e) {
      r.error = message(e);
    }
    r.elapsed_ms = performance.now() - started;
    out.key_systems.push(r);
  }
  return JSON.stringify(out);
}))BOSONJS";

[[nodiscard]] bool is_usable(const DrmKeySystemResult& r) noexcept {
    return r.supported && r.media_keys_created != std::optional<bool>{false};
}

[[nodiscard]] bool is_clear_key(const DrmKeySystemResult& r) noexcept {
    return r.key_system == "org.w3.clearkey";
}

void finish(DrmProbeResult& result) {
    result.protection = std::string(to_string(summarize(result)));
    result.best.clear();
    for (const auto& r : result.key_systems) {
        if (is_usable(r) && r.hardware) {
            result.best = r.id;
            return;
        }
    }
    for (const auto& r : result.key_systems) {
        if (is_usable(r)) {
            result.best = r.id;
            return;
        }
    }
}

} // namespace

std::vector<DrmKeySystemQuery> default_drm_key_systems() {
    return {
        {.id = "playready_sl3000", .key_system = "com.microsoft.playready.recommendation.3000", .robustness = "3000",
         .audio_robustness = "", .hardware = true},
        {.id = "playready_sl2000", .key_system = "com.microsoft.playready.recommendation", .robustness = "2000",
         .audio_robustness = "", .hardware = false},
        {.id = "widevine_l1", .key_system = "com.widevine.alpha", .robustness = "HW_SECURE_ALL",
         .audio_robustness = "SW_SECURE_CRYPTO", .hardware = true},
        {.id = "widevine_l3", .key_system = "com.widevine.alpha", .robustness = "SW_SECURE_DECODE",
         .audio_robustness = "SW_SECURE_CRYPTO", .hardware = false},
        {.id = "clearkey", .key_system = "org.w3.clearkey", .robustness = "", .audio_robustness = "", .hardware = false},
    };
}

std::vector<DrmCodec> default_drm_video_codecs() {
    return {
        {.name = "avc1", .content_type = R"(video/mp4; codecs="avc1.640028")"},       // H.264 High 4.0 (1080p)
        {.name = "hev1", .content_type = R"(video/mp4; codecs="hev1.1.6.L120.90")"},  // HEVC Main 4.0
        {.name = "vp09", .content_type = R"(video/mp4; codecs="vp09.00.40.08")"},     // VP9 profile 0, level 4
        {.name = "av01", .content_type = R"(video/mp4; codecs="av01.0.08M.08")"},     // AV1 Main 4.0, 8-bit
    };
}

std::vector<DrmCodec> default_drm_audio_codecs() {
    return {
        {.name = "mp4a", .content_type = R"(audio/mp4; codecs="mp4a.40.2")"},
        {.name = "ec-3", .content_type = R"(audio/mp4; codecs="ec-3")"},
        {.name = "opus", .content_type = R"(audio/mp4; codecs="opus")"},
    };
}

std::string_view to_string(DrmProtection protection) noexcept {
    switch (protection) {
    case DrmProtection::Hardware:
        return "hardware";
    case DrmProtection::Software:
        return "software";
    case DrmProtection::ClearKeyOnly:
        return "clearkey_only";
    case DrmProtection::None:
        break;
    }
    return "none";
}

DrmProtection summarize(const DrmProbeResult& result) noexcept {
    bool software = false;
    bool clear_key = false;
    for (const auto& r : result.key_systems) {
        if (!is_usable(r)) {
            continue;
        }
        if (r.hardware) {
            return DrmProtection::Hardware;
        }
        if (is_clear_key(r)) {
            clear_key = true;
        } else {
            software = true;
        }
    }
    if (software) {
        return DrmProtection::Software;
    }
    return clear_key ? DrmProtection::ClearKeyOnly : DrmProtection::None;
}

std::string build_drm_probe_script(const DrmProbeOptions& options) {
    std::string config;
    if (glz::write_json(options, config)) {
        config = "{}";
    }
    std::string script;
    script.reserve(kProbeFunction.size() + config.size() + 4);
    script += kProbeFunction;
    script += '(';
    script += config;
    script += ')';
    return script;
}

std::optional<DrmProbeResult> parse_drm_probe_result(std::string_view json) {
    DrmProbeResult result;
    if (glz::read<kLenient>(result, json)) {
        return std::nullopt;
    }
    finish(result);
    return result;
}

bool probe_drm(core::WebView& webview, std::function<void(HRESULT, DrmProbeResult)> callback) {
    return probe_drm(webview, DrmProbeOptions{}, std::move(callback));
}

bool probe_drm(core::WebView& webview, const DrmProbeOptions& options, std::function<void(HRESULT, DrmProbeResult)> callback) {
    ICoreWebView2* core = webview.core_webview();
    if (core == nullptr) {
        return false;
    }
    const detail::RuntimeEvaluateParams params{.expression = build_drm_probe_script(options)};
    std::string params_json;
    if (glz::write_json(params, params_json)) {
        return false;
    }
    const std::wstring params_wide = core::utf8_to_wide(params_json);
    auto shared = std::make_shared<std::function<void(HRESULT, DrmProbeResult)>>(std::move(callback));
    return BOSON_HR_CHECK(core->CallDevToolsProtocolMethod(
        L"Runtime.evaluate",
        params_wide.c_str(),
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([shared](HRESULT hr, LPCWSTR json) -> HRESULT {
            DrmProbeResult result;
            if (SUCCEEDED(hr) && json != nullptr) {
                detail::RuntimeEvaluateResponse response;
                const std::string text = core::wide_to_utf8(json);
                if (glz::read<kLenient>(response, text)) {
                    hr = E_UNEXPECTED;
                    result.error = "Runtime.evaluate returned malformed JSON";
                } else if (response.exception_details.has_value()) {
                    hr = E_FAIL;
                    result.error = "DRM probe script threw: " +
                        (response.result.description.empty() ? response.exception_details->text : response.result.description);
                } else {
                    std::string payload;
                    if (response.result.type != "string" || glz::read_json(payload, response.result.value.str)) {
                        hr = E_UNEXPECTED;
                        result.error = "DRM probe returned a " + response.result.type + " instead of a JSON string";
                    } else if (auto parsed = parse_drm_probe_result(payload)) {
                        result = std::move(*parsed);
                    } else {
                        hr = E_UNEXPECTED;
                        result.error = "DRM probe returned malformed JSON";
                    }
                }
            } else if (SUCCEEDED(hr)) {
                hr = E_UNEXPECTED;
            }
            if (FAILED(hr)) {
                core::hr_check(hr, "webview::probe_drm (Runtime.evaluate)");
            } else {
                finish(result);
            }
            if (*shared) {
                (*shared)(hr, std::move(result));
            }
            return S_OK;
        }).Get()));
}

exec::CallbackSender<DrmProbeResult> probe_drm(core::WebView& webview, DrmProbeOptions options) {
    return exec::CallbackSender<DrmProbeResult>(
        [&webview, options = std::move(options)](exec::Completion<DrmProbeResult> done) {
            if (!probe_drm(webview, options, [done](HRESULT hr, DrmProbeResult result) { done(hr, std::move(result)); })) {
                done(E_NOT_VALID_STATE, {});
            }
        },
        "webview::probe_drm");
}

} // namespace boson::webview
