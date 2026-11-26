// boson_drm_spike: hardware-DRM verification for issue #15 (docs/COMPOSITION_DRM_SPIKE.md).
//
// Plays public PlayReady test streams in Boson WebViews (windowed and composition hosting), runs
// webview::probe_drm, and records for each case whether playback advanced, how many frames were decoded and
// presented, and how much of the video area is non-black in five captures: DXGI Desktop Duplication, PrintWindow
// (PW_RENDERFULLCONTENT), ICoreWebView2::CapturePreview, and Windows.Graphics.Capture of the window and of the
// monitor (what Discord, Teams and OBS use to share a window or a screen). It also covers fullscreen playback
// (element.requestFullscreen -> borderless window over the whole monitor) and background playback (minimized,
// covered by another window). Hardware-protected (SL3000) frames are excluded
// from every capture, so "plays + black captures" is the expected hardware signature and "plays + visible
// captures" the software one. Needs network access, a physical GPU and a real display (not RDP).
//
// Usage: boson_drm_spike [--out DIR] [--modes windowed,composition] [--rounds N] [--seconds N] [--skip-matrix] [--skip-user]
//                        [--skip-flags]
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/wrappers/corewrappers.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace drm_spike {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

// ---- Test content -----------------------------------------------------------------------------------------
// Microsoft PlayReady test server: Tears of Steel (AVC, CENC). The licence cfg sets the minimum security level
// the client must have: sl:3000 is only granted to an SL3000 (hardware) client, sl:150 to anyone.
constexpr std::string_view kPlayReadyMpd = "https://test.playready.microsoft.com/media/profficialsite/tearsofsteel_4k.ism/manifest.mpd";
constexpr std::string_view kPlayReadyLicenseSl3000 = "https://test.playready.microsoft.com/service/rightsmanager.asmx?cfg=(persist:false,sl:3000)";
constexpr std::string_view kPlayReadyLicenseSl150 = "https://test.playready.microsoft.com/service/rightsmanager.asmx?cfg=(persist:false,sl:150)";
constexpr std::string_view kPlayReadySl3000 = "com.microsoft.playready.recommendation.3000";
constexpr std::string_view kPlayReadySl2000 = "com.microsoft.playready.recommendation";
// Axinom multi-DRM test vector (Widevine licence needs the entitlement header below).
constexpr std::string_view kAxinomMpd = "https://media.axprod.net/TestVectors/v7-MultiDRM-SingleKey/Manifest.mpd";
constexpr std::string_view kAxinomWidevineLicense = "https://drm-widevine-licensing.axtest.net/AcquireLicense";
constexpr std::string_view kAxinomPlayReadyLicense = "https://drm-playready-licensing.axtest.net/AcquireLicense";
constexpr std::string_view kAxinomToken =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ2ZXJzaW9uIjoxLCJjb21fa2V5X2lkIjoiYjMzNjRlYjUtNTFmNi00YWUzLThjOTgtMzNjZWQ1ZTMxYzc4IiwibWVzc2FnZSI6eyJ0eXBlIjoiZW50aXRsZW1lbnRfbWVzc2FnZSIsImtleXMiOlt7ImlkIjoiOWViNDA1MGQtZTQ0Yi00ODAyLTkzMmUtMjdkNzUwODNlMjY2IiwiZW5jcnlwdGVkX2tleSI6ImxLM09qSExZVzI0Y3Iya3RSNzRmbnc9PSJ9XX19.4lWwW46k-oWcah8oN18LPj5OLS5ZU-_AQv7fe0JhNjA";

constexpr std::string_view kPlayerHtml = R"BOSONHTML(<!doctype html><html><head><meta charset="utf-8"><title>player</title>
<script src="https://cdn.jsdelivr.net/npm/shaka-player@4.16.58/dist/shaka-player.compiled.debug.js"></script>
<style>html,body{margin:0;height:100%;background:#000;overflow:hidden}video{position:absolute;inset:0;width:100%;height:100%;object-fit:fill;background:#000}
#hud{position:absolute;left:4px;top:4px;color:#3f3;font:11px monospace;background:rgba(0,0,0,.6);padding:2px 4px;max-width:45%;white-space:pre-wrap}</style></head>
<body><video id="v" playsinline></video><div id="hud">loading</div><script>
window.__drm = { state: 'init', error: '', key_system: '', video_robustness: '', license_requests: 0, license_responses: 0, muted_fallback: false, log: [] };
for (const level of ['error', 'warn']) {
  const original = console[level].bind(console);
  console[level] = (...args) => { __drm.log.push(level + ': ' + args.map(String).join(' ').slice(0, 300)); if (__drm.log.length > 20) { __drm.log.shift(); } original(...args); };
}
window.addEventListener('error', (e) => __drm.log.push('window.onerror: ' + e.message));
window.__rvfc = 0;
const v = document.getElementById('v');
const hud = document.getElementById('hud');
const tick = () => { window.__rvfc++; v.requestVideoFrameCallback(tick); };
v.requestVideoFrameCallback(tick);
window.__sample = () => {
  const q = v.getVideoPlaybackQuality();
  return JSON.stringify({ t: v.currentTime, w: v.videoWidth, h: v.videoHeight, ready_state: v.readyState, paused: v.paused,
    total_frames: q.totalVideoFrames, dropped_frames: q.droppedVideoFrames, rvfc: window.__rvfc,
    audio_bytes: v.webkitAudioDecodedByteCount || 0, muted: v.muted,
    media_error: v.error ? (v.error.code + ' ' + v.error.message) : '', drm: window.__drm });
};
setInterval(() => { hud.textContent = __drm.state + ' ' + __drm.key_system + ' ' + v.videoWidth + 'x' + v.videoHeight + ' t=' + v.currentTime.toFixed(1) + (__drm.error ? '\n' + __drm.error : ''); }, 500);
(async () => {
  try {
    const q = new URLSearchParams(location.search);
    const ks = q.get('ks'), lic = q.get('lic'), mpd = q.get('mpd'), vr = q.get('vr') || '';
    const start = Number(q.get('start') || '0'), maxh = Number(q.get('maxh') || '1080');
    shaka.polyfill.installAll();
    if (shaka.log && shaka.log.setLevel) { shaka.log.setLevel(shaka.log.Level.WARNING); }
    v.addEventListener('error', () => __drm.log.push('video error ' + (v.error ? v.error.code + ' ' + v.error.message : '')));
    if (!shaka.Player.isBrowserSupported()) { throw new Error('shaka: browser not supported'); }
    const p = new shaka.Player();
    await p.attach(v);
    window.__player = p;
    p.addEventListener('error', (e) => { __drm.state = 'error'; __drm.error = 'shaka ' + e.detail.code + ' ' + JSON.stringify(e.detail.data).slice(0, 500); });
    const LICENSE = shaka.net.NetworkingEngine.RequestType.LICENSE;
    p.getNetworkingEngine().registerRequestFilter((type, req) => {
      if (type === LICENSE) {
        __drm.license_requests++;
        if (q.get('hk')) { req.headers[q.get('hk')] = q.get('hv'); }
      }
    });
    p.getNetworkingEngine().registerResponseFilter((type) => { if (type === LICENSE) { __drm.license_responses++; } });
    const advanced = {};
    advanced[ks] = { videoRobustness: vr ? [vr] : [], audioRobustness: [] };
    const servers = { [ks]: lic };
    if (ks.startsWith('com.microsoft.playready')) { servers['com.microsoft.playready'] = lic; advanced['com.microsoft.playready'] = advanced[ks]; }
    p.configure({ drm: { servers, keySystemsMapping: ks.startsWith('com.microsoft.playready') ? { 'com.microsoft.playready': ks } : {}, preferredKeySystems: [ks], advanced },
      abr: { enabled: false }, streaming: { bufferingGoal: 12, rebufferingGoal: 2 } });
    __drm.state = 'loading';
    await p.load(mpd, start);
    const tracks = p.getVariantTracks().filter((t) => t.height <= maxh).sort((a, b) => (b.height - a.height) || (b.bandwidth - a.bandwidth));
    if (tracks.length) { p.selectVariantTrack(tracks[0], true); }
    const info = p.drmInfo();
    __drm.key_system = p.keySystem();
    __drm.video_robustness = info ? String(info.videoRobustness || '') : '';
    __drm.state = 'loaded';
    try { await v.play(); } catch (e) {
      __drm.log.push('play() rejected: ' + (e && e.name) + ' ' + (e && e.message));
      __drm.muted_fallback = true; v.muted = true; await v.play();
      setTimeout(() => { v.muted = false; __drm.log.push('unmuted after start, muted=' + v.muted); }, 1500);
    }
    if (__drm.state !== 'error') { __drm.state = 'playing'; }
  } catch (e) {
    __drm.state = 'error';
    __drm.error = (e && e.code) ? ('shaka ' + e.code + ' ' + JSON.stringify(e.data).slice(0, 500)) : String((e && e.message) || e);
  }
})();
</script></body></html>)BOSONHTML";
constexpr std::string_view kOverlayHtml = R"BOSONHTML(<!doctype html><html><head><meta charset="utf-8"><title>overlay</title><style>html,body{margin:0;height:100%;background:transparent;overflow:hidden}
.panel{position:absolute;inset:8px;border-radius:10px;background:rgba(40,120,255,.55);color:#fff;font:bold 28px sans-serif;display:flex;align-items:center;justify-content:center}</style></head>
<body><div class="panel">Shell UI overlay (sibling WebView)</div></body></html>)BOSONHTML";
constexpr std::string_view kBlankHtml = R"BOSONHTML(<!doctype html><html><head><meta charset="utf-8"><title>probe</title></head><body style="background:#222;color:#ccc;font:14px sans-serif">DRM probe page</body></html>)BOSONHTML";

std::string to_utf8(const std::wstring& w);

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/player.html") {
        return as_bytes(kPlayerHtml);
    }
    if (path == "/overlay.html") {
        return as_bytes(kOverlayHtml);
    }
    if (path == "/blank.html") {
        return as_bytes(kBlankHtml);
    }
    return std::nullopt;
}

std::string url_encode(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4]{};
            std::snprintf(buf, sizeof(buf), "%%%02X", u);
            out += buf;
        }
    }
    return out;
}

struct Stream {
    std::string label;
    std::string key_system;
    std::string license;
    std::string mpd;
    std::string robustness;
    std::string header_name;
    std::string header_value;
    std::string start; ///< Seconds; empty = --start (default 120).
};

/// The main hardware case: Axinom 1080p AVC CENC test vector with the PlayReady SL3000 key system.
Stream playready_sl3000() {
    return {.label = "Axinom 1080p, SL3000 key system", .key_system = std::string(kPlayReadySl3000),
            .license = std::string(kAxinomPlayReadyLicense), .mpd = std::string(kAxinomMpd), .robustness = "3000",
            .header_name = "X-AxDRM-Message", .header_value = std::string(kAxinomToken)};
}

/// Microsoft test server, licence restricted to SL3000 clients (only a hardware CDM gets it).
Stream playready_ms_sl3000() {
    return {.label = "MS test server ToS, sl:3000 licence, SL3000 key system", .key_system = std::string(kPlayReadySl3000),
            .license = std::string(kPlayReadyLicenseSl3000), .mpd = std::string(kPlayReadyMpd), .robustness = "3000"};
}

Stream playready_sl2000_key_system(std::string_view license, std::string label) {
    // Microsoft test server content (the Axinom licence would work too; this one lets sl:150 vs sl:3000 be compared).
    return {.label = std::move(label), .key_system = std::string(kPlayReadySl2000), .license = std::string(license),
            .mpd = std::string(kPlayReadyMpd), .robustness = "2000"};
}

Stream widevine_l3() {
    return {.label = "Widevine SW_SECURE_DECODE", .key_system = "com.widevine.alpha", .license = std::string(kAxinomWidevineLicense),
            .mpd = std::string(kAxinomMpd), .robustness = "SW_SECURE_DECODE", .header_name = "X-AxDRM-Message",
            .header_value = std::string(kAxinomToken)};
}

int g_round = 1;
struct Report;
Report* g_report = nullptr;
std::filesystem::path g_results_md;
void write_markdown(const Report& report, const std::filesystem::path& path);
std::string g_start = "120";
std::string g_maxh = "1080";

std::string player_url(const Stream& s) {
    std::string url = "boson://app/player.html?start=" + (s.start.empty() ? g_start : s.start) + "&maxh=" + g_maxh + "&ks=" + url_encode(s.key_system) + "&lic=" + url_encode(s.license) +
                      "&mpd=" + url_encode(s.mpd) + "&vr=" + url_encode(s.robustness);
    if (!s.header_name.empty()) {
        url += "&hk=" + url_encode(s.header_name) + "&hv=" + url_encode(s.header_value);
    }
    return url;
}

// ---- Message pumping ---------------------------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    pump_until([until]() { return std::chrono::steady_clock::now() >= until; }, duration + 1s);
}

/// Evaluates js (which must return a string) and returns the decoded string.
std::optional<std::string> eval_string(boson::core::WebView& webview, std::string_view js) {
    bool done = false;
    std::optional<std::string> raw;
    if (!webview.execute_script(js, [&](HRESULT hr, std::string value) {
            if (SUCCEEDED(hr)) {
                raw = std::move(value);
            }
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, 10s);
    if (!raw.has_value() || raw->empty() || raw->front() != '"') {
        return std::nullopt;
    }
    std::string decoded;
    if (glz::read_json(decoded, *raw)) {
        return std::nullopt;
    }
    return decoded;
}

// ---- Player samples ----------------------------------------------------------------------------------------

struct DrmState {
    std::string state;
    std::string error;
    std::string key_system;
    std::string video_robustness;
    int license_requests = 0;
    int license_responses = 0;
    bool muted_fallback = false;
    std::vector<std::string> log;
};

struct Sample {
    double t = 0.0;
    int w = 0;
    int h = 0;
    int ready_state = 0;
    bool paused = true;
    std::uint64_t total_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t rvfc = 0;
    std::uint64_t audio_bytes = 0;
    bool muted = false;
    std::string media_error;
    DrmState drm;
};

std::optional<Sample> sample(boson::core::WebView& webview) {
    const auto json = eval_string(webview, "window.__sample ? window.__sample() : ''");
    if (!json.has_value() || json->empty()) {
        return std::nullopt;
    }
    Sample s;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(s, *json)) {
        return std::nullopt;
    }
    return s;
}

bool wait_player(boson::core::WebView& webview, std::chrono::milliseconds timeout, double min_t = 0.5) {
    std::optional<Sample> last;
    const bool settled = pump_until([&]() {
        last = sample(webview);
        if (last && (last->drm.state == "error" || (last->drm.state == "playing" && last->t > min_t))) {
            return true;
        }
        pump_for(250ms);
        return false;
    }, timeout);
    return settled && last && last->drm.state == "playing";
}

// ---- Captures ----------------------------------------------------------------------------------------------

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> bgra; // top-down
};

Image grab_dc(int width, int height, const std::function<void(HDC)>& paint) {
    Image img;
    if (width <= 0 || height <= 0) {
        return img;
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib != nullptr && bits != nullptr) {
        HGDIOBJ old = SelectObject(mem, dib);
        paint(mem);
        GdiFlush();
        img.width = width;
        img.height = height;
        img.bgra.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        std::memcpy(img.bgra.data(), bits, img.bgra.size() * sizeof(std::uint32_t));
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

Image grab_screen(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    return grab_dc(client.right, client.bottom, [&](HDC mem) {
        HDC screen = GetDC(nullptr);
        BitBlt(mem, 0, 0, client.right, client.bottom, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
    });
}

/// The client area as DXGI Desktop Duplication sees it: what screen-capture / screen-share apps receive.
/// Protected (hardware DRM) surfaces are black here by design. Falls back to a GDI BitBlt when unavailable.
Image grab_duplication(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return grab_screen(hwnd);
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (desc.Monitor != monitor) {
                output.Reset();
                continue;
            }
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            ComPtr<IDXGIOutput1> output1;
            ComPtr<IDXGIOutputDuplication> duplication;
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) ||
                FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
                return grab_screen(hwnd);
            }
            ComPtr<IDXGIResource> resource;
            DXGI_OUTDUPL_FRAME_INFO info{};
            HRESULT hr = E_FAIL;
            for (int attempt = 0; attempt < 10; ++attempt) {
                hr = duplication->AcquireNextFrame(200, &info, &resource);
                if (SUCCEEDED(hr) && info.LastPresentTime.QuadPart != 0) {
                    break;
                }
                if (SUCCEEDED(hr)) {
                    resource.Reset();
                    duplication->ReleaseFrame();
                }
            }
            if (FAILED(hr) || !resource) {
                return grab_screen(hwnd);
            }
            ComPtr<ID3D11Texture2D> frame;
            resource.As(&frame);
            D3D11_TEXTURE2D_DESC td{};
            frame->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            Image img;
            if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging))) {
                context->CopyResource(staging.Get(), frame.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const int ox = origin.x - desc.DesktopCoordinates.left;
                    const int oy = origin.y - desc.DesktopCoordinates.top;
                    img.width = client.right;
                    img.height = client.bottom;
                    img.bgra.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), 0);
                    const auto* base = static_cast<const std::byte*>(mapped.pData);
                    for (int y = 0; y < img.height; ++y) {
                        const int sy = oy + y;
                        if (sy < 0 || sy >= static_cast<int>(td.Height)) {
                            continue;
                        }
                        for (int x = 0; x < img.width; ++x) {
                            const int sx = ox + x;
                            if (sx < 0 || sx >= static_cast<int>(td.Width)) {
                                continue;
                            }
                            std::uint32_t px = 0;
                            std::memcpy(&px, base + static_cast<std::size_t>(sy) * mapped.RowPitch + static_cast<std::size_t>(sx) * 4, 4);
                            img.bgra[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)] = px;
                        }
                    }
                    context->Unmap(staging.Get(), 0);
                }
            }
            duplication->ReleaseFrame();
            if (img.width == 0) {
                return grab_screen(hwnd);
            }
            return img;
        }
        adapter.Reset();
    }
    return grab_screen(hwnd);
}

Image grab_print_window(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    return grab_dc(client.right, client.bottom, [&](HDC mem) { PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT); });
}

/// Copies the client area of `hwnd` out of a captured texture whose top-left is at `texture_origin` (screen pixels).
Image crop_client(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, HWND hwnd, POINT texture_origin) {
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    td.MipLevels = 1;
    td.ArraySize = 1;
    ComPtr<ID3D11Texture2D> staging;
    Image img;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) {
        return img;
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return img;
    }
    const int ox = static_cast<int>(origin.x - texture_origin.x);
    const int oy = static_cast<int>(origin.y - texture_origin.y);
    img.width = static_cast<int>(client.right);
    img.height = static_cast<int>(client.bottom);
    img.bgra.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), 0);
    const auto* base = static_cast<const std::byte*>(mapped.pData);
    for (int y = 0; y < img.height; ++y) {
        const int sy = oy + y;
        if (sy < 0 || sy >= static_cast<int>(td.Height)) {
            continue;
        }
        for (int x = 0; x < img.width; ++x) {
            const int sx = ox + x;
            if (sx < 0 || sx >= static_cast<int>(td.Width)) {
                continue;
            }
            std::uint32_t px = 0;
            std::memcpy(&px, base + static_cast<std::size_t>(sy) * mapped.RowPitch + static_cast<std::size_t>(sx) * 4, 4);
            img.bgra[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)] = px;
        }
    }
    context->Unmap(staging.Get(), 0);
    return img;
}

/// Windows.Graphics.Capture (WGC): the capture API that Discord, Teams, OBS ("Windows 10 capture"), Snipping Tool
/// and Xbox Game Bar use to share a window (`whole_monitor = false`) or a whole screen (`true`). Returns the
/// window's client area; an empty Image (and `failure`) when WGC is unavailable or delivered no frame.
Image grab_wgc(HWND hwnd, bool whole_monitor, std::string& failure) {
    namespace wgc = ABI::Windows::Graphics::Capture;
    namespace wgd = ABI::Windows::Graphics::DirectX;
    using Microsoft::WRL::Wrappers::HStringReference;
    const auto fail = [&](std::string why, HRESULT hr) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        failure = std::move(why) + " " + code;
        return Image{};
    };
    ComPtr<IGraphicsCaptureItemInterop> interop;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(), IID_PPV_ARGS(&interop));
    if (FAILED(hr)) {
        return fail("GraphicsCaptureItem factory", hr);
    }
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ComPtr<wgc::IGraphicsCaptureItem> item;
    hr = whole_monitor ? interop->CreateForMonitor(monitor, IID_PPV_ARGS(&item)) : interop->CreateForWindow(hwnd, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        return fail("CreateFor" + std::string(whole_monitor ? "Monitor" : "Window"), hr);
    }
    ABI::Windows::Graphics::SizeInt32 size{};
    item->get_Size(&size);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                           &context);
    if (FAILED(hr)) {
        return fail("D3D11CreateDevice", hr);
    }
    ComPtr<IDXGIDevice> dxgi_device;
    device.As(&dxgi_device);
    ComPtr<IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable);
    ComPtr<wgd::Direct3D11::IDirect3DDevice> d3d_device;
    if (FAILED(hr) || FAILED(inspectable.As(&d3d_device))) {
        return fail("CreateDirect3D11DeviceFromDXGIDevice", hr);
    }
    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
    hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool).Get(), IID_PPV_ARGS(&pool_statics));
    if (FAILED(hr)) {
        return fail("Direct3D11CaptureFramePool factory", hr);
    }
    ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
    hr = pool_statics->CreateFreeThreaded(d3d_device.Get(), wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, size, &pool);
    if (FAILED(hr)) {
        return fail("CreateFreeThreaded", hr);
    }
    ComPtr<wgc::IGraphicsCaptureSession> session;
    hr = pool->CreateCaptureSession(item.Get(), &session);
    if (FAILED(hr)) {
        return fail("CreateCaptureSession", hr);
    }
    if (ComPtr<wgc::IGraphicsCaptureSession2> s2; SUCCEEDED(session.As(&s2))) {
        s2->put_IsCursorCaptureEnabled(FALSE);
    }
    if (ComPtr<wgc::IGraphicsCaptureSession3> s3; SUCCEEDED(session.As(&s3))) {
        s3->put_IsBorderRequired(FALSE); // best effort: needs GraphicsCaptureAccess, so the yellow border may still show
    }
    hr = session->StartCapture();
    if (FAILED(hr)) {
        return fail("StartCapture", hr);
    }
    // Keep the newest frame after a short warm-up: the first frame can predate the current video frame.
    ComPtr<wgc::IDirect3D11CaptureFrame> latest;
    const auto started = std::chrono::steady_clock::now();
    pump_until([&]() {
        ComPtr<wgc::IDirect3D11CaptureFrame> next;
        if (SUCCEEDED(pool->TryGetNextFrame(&next)) && next) {
            latest = next;
        }
        return latest && std::chrono::steady_clock::now() - started > 500ms;
    }, 3s);
    Image img;
    if (latest) {
        ComPtr<wgd::Direct3D11::IDirect3DSurface> surface;
        ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(latest->get_Surface(&surface)) && SUCCEEDED(surface.As(&access)) && SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&texture)))) {
            POINT texture_origin{0, 0};
            if (whole_monitor) {
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                GetMonitorInfoW(monitor, &mi);
                texture_origin = POINT{mi.rcMonitor.left, mi.rcMonitor.top};
            } else {
                // A window capture starts at the window's visible frame (DWM extended frame bounds).
                RECT frame{};
                if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
                    GetWindowRect(hwnd, &frame);
                }
                texture_origin = POINT{frame.left, frame.top};
            }
            img = crop_client(device.Get(), context.Get(), texture.Get(), hwnd, texture_origin);
        }
        latest.Reset();
    } else {
        failure = "no WGC frame within 3 s";
    }
    if (ComPtr<ABI::Windows::Foundation::IClosable> closable; SUCCEEDED(session.As(&closable))) {
        closable->Close();
    }
    if (ComPtr<ABI::Windows::Foundation::IClosable> closable; SUCCEEDED(pool.As(&closable))) {
        closable->Close();
    }
    return img;
}

ComPtr<IWICImagingFactory> wic() {
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        BOSON_HR_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    }
    return factory;
}

Image decode_png(const std::vector<std::byte>& png) {
    Image img;
    const auto factory = wic();
    if (!factory || png.empty()) {
        return img;
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(png.data()), static_cast<UINT>(png.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0;
    UINT h = 0;
    if (!stream || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&w, &h))) {
        return img;
    }
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.bgra.resize(static_cast<std::size_t>(w) * h);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(img.bgra.size() * 4), reinterpret_cast<BYTE*>(img.bgra.data())))) {
        img = {};
    }
    return img;
}

void save_png(const Image& img, const std::filesystem::path& path) {
    const auto factory = wic();
    if (!factory || img.width == 0) {
        return;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr))) {
        return;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height));
    frame->SetPixelFormat(&format);
    if (format != GUID_WICPixelFormat32bppBGRA) {
        return;
    }
    std::vector<std::uint32_t> copy = img.bgra;
    for (auto& p : copy) {
        p |= 0xFF000000u; // GDI leaves alpha undefined
    }
    frame->WritePixels(static_cast<UINT>(img.height), static_cast<UINT>(img.width) * 4, static_cast<UINT>(copy.size() * 4),
                       reinterpret_cast<BYTE*>(copy.data()));
    frame->Commit();
    encoder->Commit();
}

/// Percentage of pixels in the central part of r (inner 60% x 50%, away from the HUD) with any channel > 10.
double visible_percent(const Image& img, RECT r) {
    if (img.width == 0) {
        return -1.0;
    }
    const int left = static_cast<int>(r.left);
    const int top = static_cast<int>(r.top);
    const int rw = static_cast<int>(r.right) - left;
    const int rh = static_cast<int>(r.bottom) - top;
    const int x0 = std::clamp(left + rw / 5, 0, img.width);
    const int x1 = std::clamp(left + rw - rw / 5, 0, img.width);
    const int y0 = std::clamp(top + rh / 4, 0, img.height);
    const int y1 = std::clamp(top + rh - rh / 4, 0, img.height);
    std::uint64_t lit = 0;
    std::uint64_t total = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::uint32_t p = img.bgra[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)];
            const auto b = p & 0xFFu;
            const auto g = (p >> 8) & 0xFFu;
            const auto rr = (p >> 16) & 0xFFu;
            lit += (b > 10 || g > 10 || rr > 10) ? 1 : 0;
            ++total;
        }
    }
    return total == 0 ? -1.0 : 100.0 * static_cast<double>(lit) / static_cast<double>(total);
}

std::optional<Image> capture_preview_image(boson::core::WebView& webview) {
    std::optional<HRESULT> hr;
    std::vector<std::byte> png;
    if (!boson::webview::capture_preview(webview, boson::webview::ImageFormat::Png, [&](HRESULT h, std::vector<std::byte> bytes) {
            hr = h;
            png = std::move(bytes);
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return hr.has_value(); }, 10s);
    if (!hr.has_value() || FAILED(*hr)) {
        return std::nullopt;
    }
    return decode_png(png);
}

// ---- Results -----------------------------------------------------------------------------------------------

struct CaseResult {
    int round = 1;
    std::string mode;
    std::string name;
    std::string stream;
    std::string key_system;
    std::string robustness;
    std::string state;
    bool plays = false;
    double advanced_s = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t dropped = 0;
    std::uint64_t presented = 0;
    int width = 0;
    int height = 0;
    bool audio_decoding = false;
    std::optional<bool> webview_playing_audio;
    bool occluded = false; ///< Another window covered the centre of the video when the screen was grabbed.
    double screen_visible = -1.0;
    double print_window_visible = -1.0;
    double preview_visible = -1.0;
    double wgc_window_visible = -1.0;  ///< Windows.Graphics.Capture of the window (Discord/Teams "share window").
    double wgc_monitor_visible = -1.0; ///< Windows.Graphics.Capture of the monitor (Discord/Teams "share screen").
    std::string verdict;
    std::string note;
    std::string error;
};

struct ProbeRow {
    std::string context;
    std::string protection;
    std::string best;
    std::vector<std::string> supported; ///< "id(codecs; keys; hdcp)"
    std::vector<std::string> unsupported;
};

struct Report {
    std::string os_build;
    std::string runtime;
    std::string gpu;
    std::vector<CaseResult> cases;
    std::vector<ProbeRow> probes;
};

std::string fmt(double v, int digits = 1) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

std::string verdict_of(const CaseResult& r) {
    if (!r.plays) {
        return "no playback";
    }
    const double best_capture = std::max({r.occluded ? -1.0 : r.screen_visible, r.print_window_visible, r.preview_visible, r.wgc_window_visible,
                                          r.occluded ? -1.0 : r.wgc_monitor_visible});
    if (best_capture >= 0.0 && best_capture < 3.0) {
        return "plays, black in all captures (protected path)";
    }
    if (std::max({r.print_window_visible, r.occluded ? -1.0 : r.screen_visible, r.wgc_window_visible}) >= 20.0) {
        return "plays, visible in captures (not protected)";
    }
    return "plays, partly captured";
}

/// Samples, waits, samples again and captures. bounds = the WebView's rect in client pixels.
[[noreturn]] void abort_run(std::string_view why) {
    std::cout << "ABORT: " << why << '\n' << std::flush;
    if (g_report != nullptr) {
        write_markdown(*g_report, g_results_md);
    }
    std::exit(3);
}

CaseResult measure(std::string mode, std::string name, const Stream& stream, boson::core::Window& window, boson::core::WebView& webview,
                   std::chrono::milliseconds window_len, const std::filesystem::path& out_dir, std::string note = {},
                   std::optional<RECT> overlay = std::nullopt) {
    if (IsWindow(window.hwnd()) == FALSE) {
        abort_run("the test window was closed from outside");
    }
    CaseResult r{.round = g_round, .mode = std::move(mode), .name = std::move(name), .stream = stream.label, .note = std::move(note)};
    const auto s0 = sample(webview);
    pump_for(window_len);
    const auto s1 = sample(webview);
    if (s1) {
        r.state = s1->drm.state;
        r.key_system = s1->drm.key_system;
        r.robustness = s1->drm.video_robustness;
        r.error = s1->drm.error;
        if (!s1->media_error.empty()) {
            r.error = "HTMLMediaElement.error " + s1->media_error + (r.error.empty() ? "" : "; " + r.error);
        }
        r.width = s1->w;
        r.height = s1->h;
        r.note += (r.note.empty() ? "" : "; ") + std::string("licence requests/responses ") + std::to_string(s1->drm.license_requests) + "/" +
                  std::to_string(s1->drm.license_responses);
        if (s1->drm.muted_fallback) {
            r.note += (r.note.empty() ? "" : "; ") + std::string("autoplay needed muted fallback");
        }
    }
    if (s0 && s1) {
        r.advanced_s = s1->t - s0->t;
        r.frames = s1->total_frames - std::min(s1->total_frames, s0->total_frames);
        r.dropped = s1->dropped_frames - std::min(s1->dropped_frames, s0->dropped_frames);
        r.presented = s1->rvfc - std::min(s1->rvfc, s0->rvfc);
        r.audio_decoding = s1->audio_bytes > s0->audio_bytes;
        const double expected = static_cast<double>(window_len.count()) / 1000.0;
        r.plays = r.advanced_s > expected * 0.6 && r.frames > 0;
    }
    r.webview_playing_audio = webview.is_playing_audio();

    const RECT b = webview.bounds();
    const std::string stem = "r" + std::to_string(g_round) + "-" + r.mode + "-" + r.name;
    std::string file;
    for (const char c : stem) {
        file += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
    }
    // Lift the window above other (non-topmost) windows for the screen captures only, without activating it,
    // so windows the user moves around during a run do not end up in the DXGI and WGC monitor captures.
    SetWindowPos(window.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pump_for(300ms);
    {
        // Occluded when any of 9 points across the sampled area belongs to another window (e.g. another topmost one).
        for (int gy = 1; gy <= 3 && !r.occluded; ++gy) {
            for (int gx = 1; gx <= 3 && !r.occluded; ++gx) {
                POINT p{b.left + (b.right - b.left) * gx / 4, b.top + (b.bottom - b.top) * gy / 4};
                ClientToScreen(window.hwnd(), &p);
                const HWND hit = WindowFromPoint(p);
                if (GetAncestor(hit, GA_ROOTOWNER) == window.hwnd()) {
                    continue;
                }
                r.occluded = true;
                wchar_t cls[128]{};
                wchar_t title[128]{};
                const HWND top = GetAncestor(hit, GA_ROOT);
                GetClassNameW(top, cls, 127);
                GetWindowTextW(top, title, 127);
                DWORD pid = 0;
                GetWindowThreadProcessId(top, &pid);
                r.note += (r.note.empty() ? "" : "; ") + std::string("screen grab occluded by '") + to_utf8(cls) + "' '" + to_utf8(title).substr(0, 40) +
                          "' pid " + std::to_string(pid);
            }
        }
    }
    const Image screen = grab_duplication(window.hwnd());
    const Image printed = grab_print_window(window.hwnd());
    r.screen_visible = visible_percent(screen, b);
    r.print_window_visible = visible_percent(printed, b);
    if (overlay.has_value()) {
        r.note += (r.note.empty() ? "" : "; ") + std::string("overlay region visible % screen/PrintWindow ") +
                  fmt(visible_percent(screen, *overlay)) + "/" + fmt(visible_percent(printed, *overlay));
    }
    save_png(screen, out_dir / (file + "-screen.png"));
    save_png(printed, out_dir / (file + "-printwindow.png"));
    {
        std::string failure;
        const Image wgc_window = grab_wgc(window.hwnd(), false, failure);
        r.wgc_window_visible = visible_percent(wgc_window, b);
        const Image wgc_monitor = grab_wgc(window.hwnd(), true, failure);
        r.wgc_monitor_visible = visible_percent(wgc_monitor, b);
        if (!failure.empty()) {
            r.note += (r.note.empty() ? "" : "; ") + std::string("WGC: ") + failure;
        }
        save_png(wgc_window, out_dir / (file + "-wgc-window.png"));
        save_png(wgc_monitor, out_dir / (file + "-wgc-monitor.png"));
    }
    SetWindowPos(window.hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (const auto preview = capture_preview_image(webview)) {
        r.preview_visible = visible_percent(*preview, RECT{0, 0, preview->width, preview->height});
        save_png(*preview, out_dir / (file + "-preview.png"));
    }
    r.verdict = verdict_of(r);
    if (s1) {
        for (const auto& line : s1->drm.log) {
            if (line.find("play() rejected") != std::string::npos || line.find("unmuted") != std::string::npos) {
                r.note += "; " + line.substr(0, 120);
            }
        }
    }
    std::cout << "  [" << r.mode << "] " << r.name << ": " << r.verdict << " | ks=" << r.key_system << " rob=" << r.robustness
              << " " << r.width << "x" << r.height << " +" << fmt(r.advanced_s) << "s frames=" << r.frames << " dropped=" << r.dropped
              << " presented=" << r.presented << " audio=" << (r.audio_decoding ? "decoding" : "-") << "/" << (r.webview_playing_audio.value_or(false) ? "playing" : "silent")
              << " capture%(screen/print/preview)=" << fmt(r.screen_visible) << "/" << fmt(r.print_window_visible) << "/"
              << fmt(r.preview_visible) << " wgc%(window/monitor)=" << fmt(r.wgc_window_visible) << "/" << fmt(r.wgc_monitor_visible)
              << (r.error.empty() ? "" : " error=" + r.error) << (r.note.empty() ? "" : " note=" + r.note) << '\n'
              << std::flush;
    return r;
}

void add_case(Report& report, CaseResult r);

ProbeRow probe(boson::core::WebView& webview, std::string context, const boson::webview::DrmProbeOptions& options = {}) {
    ProbeRow row{.context = std::move(context)};
    std::optional<HRESULT> hr;
    boson::webview::DrmProbeResult result;
    if (boson::webview::probe_drm(webview, options, [&](HRESULT h, boson::webview::DrmProbeResult r) {
            hr = h;
            result = std::move(r);
        })) {
        pump_until([&]() { return hr.has_value(); }, 120s);
    }
    if (!hr.has_value() || FAILED(*hr) || !result.error.empty()) {
        row.protection = "probe failed: " + result.error;
        std::cout << "  probe [" << row.context << "] FAILED " << result.error << '\n';
        return row;
    }
    row.protection = result.protection;
    row.best = result.best;
    for (const auto& k : result.key_systems) {
        if (k.supported) {
            std::string codecs;
            for (const auto& c : k.video) {
                if (codecs.find(c.codec) == std::string::npos) {
                    codecs += (codecs.empty() ? "" : "/") + c.codec;
                }
            }
            std::string schemes;
            for (const auto& c : k.video) {
                if (!c.encryption_scheme.empty() && schemes.find(c.encryption_scheme) == std::string::npos) {
                    schemes += (schemes.empty() ? "" : "+") + c.encryption_scheme;
                }
            }
            std::string keys = k.media_keys_created.has_value() ? (*k.media_keys_created ? "keys ok" : "createMediaKeys FAILED") : "keys -";
            row.supported.push_back(k.id + " (" + codecs + "; " + schemes + "; " + keys + "; HDCP2.2 " + (k.hdcp_2_2.empty() ? "-" : k.hdcp_2_2) + ")");
        } else {
            row.unsupported.push_back(k.id);
        }
    }
    std::cout << "  probe [" << row.context << "] protection=" << row.protection << " best=" << row.best << '\n';
    for (const auto& s : row.supported) {
        std::cout << "    + " << s << '\n';
    }
    for (const auto& s : row.unsupported) {
        std::cout << "    - " << s << '\n';
    }
    return row;
}

void add_case(Report& report, CaseResult r) {
    report.cases.push_back(std::move(r));
    if (!g_results_md.empty()) {
        write_markdown(report, g_results_md); // incremental, so a crash or an aborted run keeps what it measured
    }
}

// ---- Environment / window helpers ---------------------------------------------------------------------------

struct Rig {
    std::unique_ptr<boson::core::Window> window;
    std::shared_ptr<boson::core::Environment> env;
    std::unique_ptr<boson::core::WebViewHost> host;
    std::function<void(const boson::core::LayoutContext&)> layout;
};

std::unique_ptr<Rig> make_rig(boson::core::HostingMode mode, boson::core::EnvironmentConfig cfg, std::wstring title) {
    auto rig = std::make_unique<Rig>();
    // Lets the page play unmuted without a user gesture, so IsDocumentPlayingAudio can report audio output
    // (the WebView itself is muted with put_IsMuted, so nothing is audible).
    cfg.additional_browser_arguments.emplace_back(L"--autoplay-policy=no-user-gesture-required");
    boson::core::WindowConfig wcfg;
    wcfg.title = std::move(title);
    wcfg.width = 1280;
    wcfg.height = 760;
    wcfg.quit_on_destroy = false;
    rig->window = std::make_unique<boson::core::Window>(wcfg);
    rig->window->create(nullptr, SW_SHOWNOACTIVATE);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    // Not topmost: the run takes a while and must not sit on top of the user's work. Screen (DXGI) captures
    // record when another window covered the video; PrintWindow and CapturePreview do not depend on z-order.
    SetWindowPos(rig->window->hwnd(), nullptr, work.left, work.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    rig->env = boson::core::Environment::create(std::move(cfg));
    rig->host = std::make_unique<boson::core::WebViewHost>(*rig->window, rig->env, mode);
    rig->host->set_default_asset_lookup(&lookup);
    Rig* raw = rig.get();
    rig->host->set_layout([raw](boson::core::WebViewHost& host, const boson::core::LayoutContext& ctx) {
        if (raw->layout) {
            raw->layout(ctx);
        } else {
            for (const auto& w : host.webviews()) {
                w->set_bounds(RECT{0, 0, ctx.width(), ctx.height()});
            }
        }
    });
    return rig;
}

void shutdown_rig(std::unique_ptr<Rig>& rig) {
    if (!rig) {
        return;
    }
    bool down = false;
    rig->env->shutdown([&]() { down = true; });
    pump_until([&]() { return down; }, 15s);
    rig->host.reset();
    rig->window.reset();
    rig.reset();
}

boson::core::WebView& create_view(Rig& rig, std::string url, bool transparent = false) {
    boson::core::WebViewConfig cfg;
    cfg.initial_url = std::move(url);
    cfg.transparent_background = transparent;
    cfg.settings.devtools_enabled = true;
    boson::core::WebView& view = rig.host->create(cfg);
    pump_until([&]() { return view.is_ready(); }, 30s);
    view.set_muted(true); // WebView-level mute: the page plays unmuted (so audio is decoded) without making noise.
    rig.host->relayout();
    return view;
}

/// SL3000 playback fed from the HTTP disk cache failed (Shaka 3014) on the test machine, in Edge 158 as well,
/// so every protected case starts from an empty disk cache.
void clear_disk_cache(boson::core::WebView& view) {
    std::optional<HRESULT> hr;
    if (boson::webview::clear_browsing_data(view, boson::webview::BrowsingDataKind::DiskCache, std::nullopt, std::nullopt,
                                            [&](HRESULT h) { hr = h; })) {
        pump_until([&]() { return hr.has_value(); }, 20s);
    }
}

bool g_keep_cache = false;

bool navigate_and_wait(boson::core::WebView& view, const std::string& url, double min_t = -1.0) {
    if (!g_keep_cache) {
        view.navigate("boson://app/blank.html");
        pump_for(1s);
        clear_disk_cache(view);
    }
    view.navigate(url);
    pump_for(500ms);
    return wait_player(view, 60s, min_t >= 0.0 ? min_t : std::atof(g_start.c_str()) + 0.5);
}

std::string to_utf8(const std::wstring& w) {
    return boson::core::wide_to_utf8(w);
}

std::string gpu_description() {
    std::string out;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0) != FALSE; ++i) {
        const std::string name = to_utf8(dd.DeviceString);
        if (out.find(name) == std::string::npos) {
            out += (out.empty() ? "" : ", ") + name;
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    return out;
}

std::string os_build() {
    HKEY key = nullptr;
    std::string out = "unknown";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[128]{};
        DWORD size = sizeof(buf);
        DWORD ubr = 0;
        DWORD ubr_size = sizeof(ubr);
        if (RegQueryValueExW(key, L"CurrentBuild", nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS) {
            out = to_utf8(buf);
            if (RegQueryValueExW(key, L"UBR", nullptr, nullptr, reinterpret_cast<BYTE*>(&ubr), &ubr_size) == ERROR_SUCCESS) {
                out += "." + std::to_string(ubr);
            }
        }
        RegCloseKey(key);
    }
    return out;
}

bool g_quick = false;
bool g_custom_composition = false; ///< custom stream (--mpd ...) with --modes composition
std::string g_user_filter; ///< --user: only the fullscreen/background cases whose name or stream contains one of these words.
std::optional<Stream> g_custom;
int g_repeat = 1;
std::wstring g_udf;
bool g_blank_between = false;

void run_custom(const std::filesystem::path& root, const std::filesystem::path& out, Report& report, std::chrono::milliseconds window_len,
                const std::vector<std::wstring>& disable, const std::vector<std::wstring>& enable, const std::vector<std::wstring>& args) {
    boson::core::EnvironmentConfig cfg;
    cfg.user_data_folder = g_udf.empty() ? (root / L"udf-custom").wstring() : g_udf;
    cfg.disable_features = disable;
    cfg.enable_features = enable;
    cfg.additional_browser_arguments = args;
    const auto mode = g_custom_composition ? boson::core::HostingMode::Composition : boson::core::HostingMode::Windowed;
    auto rig = make_rig(mode, cfg, L"Boson DRM spike (custom)");
    std::cout << "  browser arguments: '" << to_utf8(rig->env->browser_arguments()) << "'\n";
    boson::core::WebView& a = create_view(*rig, player_url(*g_custom));
    wait_player(a, 60s, std::atof(g_start.c_str()) + 0.5);
    report.probes.push_back(probe(a, "custom"));
    for (int i = 0; i < g_repeat; ++i) {
        if (i > 0) {
            if (g_blank_between) {
                a.navigate("boson://app/blank.html");
                pump_for(3s);
            }
            navigate_and_wait(a, player_url(*g_custom));
        }
        add_case(report, measure(g_custom_composition ? "composition" : "windowed", "custom #" + std::to_string(i + 1), *g_custom, *rig->window, a, window_len, out));
        if (const auto s = sample(a)) {
            for (const auto& line : s->drm.log) {
                if (line.find("Rejecting variant") == std::string::npos) {
                    std::cout << "    log: " << line << '\n';
                }
            }
        }
    }
    shutdown_rig(rig);
}

// ---- Scenarios ---------------------------------------------------------------------------------------------

void run_mode(boson::core::HostingMode mode, const std::filesystem::path& root, const std::filesystem::path& out, Report& report,
              std::chrono::milliseconds window_len) {
    const std::string m = mode == boson::core::HostingMode::Windowed ? "windowed" : "composition";
    std::cout << "== " << m << " ==\n";
    boson::core::EnvironmentConfig cfg;
    cfg.user_data_folder = (root / (L"udf-" + boson::core::utf8_to_wide(m))).wstring();
    auto rig = make_rig(mode, cfg, L"Boson DRM spike (" + boson::core::utf8_to_wide(m) + L")");
    const Stream sl3000 = playready_sl3000();
    const Stream sl2000_150 = playready_sl2000_key_system(kPlayReadyLicenseSl150, "PlayReady SL2000 key system, sl:150 licence");
    const Stream sl2000_3000 = playready_sl2000_key_system(kPlayReadyLicenseSl3000, "PlayReady SL2000 key system, sl:3000 licence");

    boson::core::WebView& a = create_view(*rig, player_url(sl3000));
    std::cout << "  browser arguments: '" << to_utf8(rig->env->browser_arguments()) << "'\n";
    const bool a_plays = wait_player(a, 60s, std::atof(g_start.c_str()) + 0.5);
    report.probes.push_back(probe(a, m + ", default flags"));
    add_case(report, measure(m, "1 baseline SL3000", sl3000, *rig->window, a, window_len, out,
                                   a_plays ? "" : "player did not reach playing"));
    if (g_quick) {
        navigate_and_wait(a, player_url(playready_ms_sl3000()));
        add_case(report, measure(m, "1b MS sl:3000 licence, SL3000 key system", playready_ms_sl3000(), *rig->window, a, window_len, out));
        navigate_and_wait(a, player_url(sl2000_150));
        add_case(report, measure(m, "7 SL2000 key system, sl:150 licence", sl2000_150, *rig->window, a, window_len, out));
        const Stream wv = widevine_l3();
        navigate_and_wait(a, player_url(wv));
        add_case(report, measure(m, "11 Widevine (software CDM)", wv, *rig->window, a, window_len, out));
        shutdown_rig(rig);
        return;
    }

    const auto fresh = [&]() { return navigate_and_wait(a, player_url(sl3000)); };

    // Hidden and shown again.
    fresh();
    a.set_visible(false);
    pump_for(3s);
    const auto hidden = sample(a);
    a.set_visible(true);
    pump_for(1500ms);
    add_case(report, measure(m, "2 hide 3s then show", sl3000, *rig->window, a, window_len, out,
                                   hidden ? "state while hidden: " + hidden->drm.state + ", t=" + fmt(hidden->t) : ""));

    // Suspended and resumed.
    fresh();
    a.set_visible(false);
    std::optional<std::pair<HRESULT, bool>> suspended;
    a.try_suspend([&](HRESULT hr, bool ok) { suspended = std::make_pair(hr, ok); });
    pump_until([&]() { return suspended.has_value(); }, 10s);
    const auto is_suspended = a.is_suspended();
    pump_for(3s);
    a.resume();
    a.set_visible(true);
    pump_for(3s);
    std::string suspend_note = "TrySuspend ";
    if (!suspended.has_value()) {
        suspend_note += "did not complete";
    } else {
        char hr_text[16]{};
        std::snprintf(hr_text, sizeof(hr_text), "0x%08lX", static_cast<unsigned long>(suspended->first));
        suspend_note += std::string("hr=") + hr_text + " result=" + (suspended->second ? "suspended" : "not suspended");
    }
    suspend_note += std::string(", IsSuspended=") + (is_suspended.has_value() ? (*is_suspended ? "true" : "false") : "n/a");
    add_case(report, measure(m, "3 suspend then resume", sl3000, *rig->window, a, window_len, out, suspend_note));

    // Two protected tabs side by side.
    {
        fresh();
        boson::core::WebView* b_ptr = nullptr;
        rig->layout = [&](const boson::core::LayoutContext& ctx) {
            a.set_bounds(RECT{0, 0, ctx.width() / 2, ctx.height()});
            if (b_ptr != nullptr) {
                b_ptr->set_bounds(RECT{ctx.width() / 2, 0, ctx.width(), ctx.height()});
            }
        };
        rig->host->relayout();
        Stream other = sl3000;
        other.start = "400"; // different segments than tab A, so B is not fed from the disk cache A filled
        boson::core::WebView& b = create_view(*rig, player_url(other));
        b_ptr = &b;
        rig->host->relayout();
        const bool b_plays = wait_player(b, 60s, 400.5);
        add_case(report, measure(m, "4a two tabs (left)", sl3000, *rig->window, a, window_len, out));
        add_case(report, measure(m, "4b two tabs (right)", other, *rig->window, b, window_len, out,
                                       b_plays ? "" : "second player did not reach playing"));
        rig->host->destroy(b);
        rig->layout = {};
        pump_for(500ms);
        rig->host->relayout();
    }

    // Sibling shell-UI WebView overlapping the protected one.
    {
        fresh();
        boson::core::WebView* o_ptr = nullptr;
        RECT overlay_rect{};
        rig->layout = [&](const boson::core::LayoutContext& ctx) {
            a.set_bounds(RECT{0, 0, ctx.width(), ctx.height()});
            // Below the measured centre band of the video, so the video and the overlay are measured separately.
            overlay_rect = RECT{ctx.width() / 4, ctx.height() * 78 / 100, ctx.width() * 3 / 4, ctx.height() * 96 / 100};
            if (o_ptr != nullptr) {
                o_ptr->set_bounds(overlay_rect);
            }
        };
        boson::core::WebView& o = create_view(*rig, "boson://app/overlay.html", true);
        o_ptr = &o;
        o.bring_to_front();
        rig->host->relayout();
        pump_for(1500ms);
        add_case(report, measure(m, "5 overlapping sibling WebView", sl3000, *rig->window, a, window_len, out,
                                       "translucent overlay WebView over the lower part of the video", overlay_rect));
        rig->host->destroy(o);
        rig->layout = {};
        pump_for(500ms);
        rig->host->relayout();
    }

    if (mode == boson::core::HostingMode::Composition) {
        fresh();
        a.set_corner_radius(8.0f);
        a.set_opacity(0.95f);
        pump_for(1s);
        add_case(report, measure(m, "6 corner radius 8 + opacity 0.95", sl3000, *rig->window, a, window_len, out));
        a.set_corner_radius(0.0f);
        a.set_opacity(1.0f);
        pump_for(500ms);
    }

    // The Microsoft test server's sl:3000 licence (granted only to an SL3000 client) and software controls.
    const Stream ms3000 = playready_ms_sl3000();
    navigate_and_wait(a, player_url(ms3000));
    add_case(report, measure(m, "1b MS sl:3000 licence, SL3000 key system", ms3000, *rig->window, a, window_len, out));
    navigate_and_wait(a, player_url(sl2000_150));
    add_case(report, measure(m, "7 SL2000 key system, sl:150 licence", sl2000_150, *rig->window, a, window_len, out));
    navigate_and_wait(a, player_url(sl2000_3000));
    add_case(report, measure(m, "8 SL2000 key system, sl:3000 licence", sl2000_3000, *rig->window, a, window_len, out));

    // Environment::recreate with feature switches, then back to the default flags.
    const auto recreate = [&](boson::core::EnvironmentConfig next, const std::string& label) {
        std::optional<bool> done;
        rig->env->recreate(std::move(next), [&](bool ok) { done = ok; });
        pump_until([&]() { return done.has_value() && a.is_ready(); }, 60s);
        pump_for(1s);
        std::cout << "  recreate (" << label << ") ok=" << done.value_or(false) << ", browser arguments: '"
                  << to_utf8(rig->env->browser_arguments()) << "'\n";
        report.probes.push_back(probe(a, m + ", recreate " + label));
        return std::string("recreate ok=") + (done.value_or(false) ? "true" : "false");
    };
    {
        boson::core::EnvironmentConfig off = cfg;
        off.disable_features = {L"HardwareSecureDecryption"};
        const std::string ok = recreate(off, "disable_features={HardwareSecureDecryption}");
        navigate_and_wait(a, player_url(sl3000));
        add_case(report, measure(m, "9a HardwareSecureDecryption off: SL3000", sl3000, *rig->window, a, window_len, out, ok));
    }
    {
        boson::core::EnvironmentConfig off = cfg;
        off.enable_features = {L"msDisablePlayReadyHardwareDrm"};
        const std::string ok = recreate(off, "enable_features={msDisablePlayReadyHardwareDrm}");
        navigate_and_wait(a, player_url(sl3000));
        add_case(report, measure(m, "9b msDisablePlayReadyHardwareDrm: SL3000", sl3000, *rig->window, a, window_len, out, ok));
        navigate_and_wait(a, player_url(sl2000_150));
        add_case(report, measure(m, "9c msDisablePlayReadyHardwareDrm: SL2000 sl:150", sl2000_150, *rig->window, a, window_len, out));
    }
    {
        const std::string ok = recreate(cfg, "back to default flags");
        navigate_and_wait(a, player_url(sl3000));
        add_case(report, measure(m, "10 back to default: SL3000", sl3000, *rig->window, a, window_len, out, ok));
    }

    if (mode == boson::core::HostingMode::Windowed) {
        const Stream wv = widevine_l3();
        navigate_and_wait(a, player_url(wv));
        add_case(report, measure(m, "11 Widevine (software CDM)", wv, *rig->window, a, window_len, out));
    }
    shutdown_rig(rig);
}

/// Calls a DevTools protocol method and returns its raw JSON reply.
std::string devtools_call(boson::core::WebView& view, const wchar_t* method, const std::string& params_json) {
    ICoreWebView2* core = view.core_webview();
    if (core == nullptr) {
        return "no ICoreWebView2";
    }
    const std::wstring params = boson::core::utf8_to_wide(params_json);
    auto reply = std::make_shared<std::optional<std::string>>();
    const HRESULT hr = core->CallDevToolsProtocolMethod(
        method, params.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([reply](HRESULT result, LPCWSTR json) -> HRESULT {
            *reply = (SUCCEEDED(result) && json != nullptr) ? boson::core::wide_to_utf8(json) : std::string("Runtime.evaluate failed");
            return S_OK;
        }).Get());
    if (FAILED(hr)) {
        return "CallDevToolsProtocolMethod failed";
    }
    pump_until([&]() { return reply->has_value(); }, 10s);
    return reply->value_or("timeout");
}

/// Runtime.evaluate with userGesture and awaitPromise. Returns the raw DevTools reply.
std::string devtools_eval(boson::core::WebView& view, std::string_view js) {
    std::string expression;
    if (glz::write_json(std::string(js), expression)) {
        return "could not encode the expression";
    }
    return devtools_call(view, L"Runtime.evaluate", R"({"expression":)" + expression + R"(,"userGesture":true,"awaitPromise":true,"returnByValue":true})");
}

/// A real left click delivered to the renderer (Input.dispatchMouseEvent): a genuine user activation, which
/// element.requestFullscreen() needs.
void devtools_click(boson::core::WebView& view, int x, int y) {
    const std::string at = R"("x":)" + std::to_string(x) + R"(,"y":)" + std::to_string(y) + R"(,"button":"left","clickCount":1})";
    devtools_call(view, L"Input.dispatchMouseEvent", R"({"type":"mousePressed",)" + at);
    devtools_call(view, L"Input.dispatchMouseEvent", R"({"type":"mouseReleased",)" + at);
}

/// Enters element fullscreen the way a user does on a streaming site: a click whose handler calls
/// video.requestFullscreen(). Returns how it went.
std::string enter_fullscreen(boson::core::WebView& view, boson::core::Window& window) {
    const auto attempt = [&]() {
        eval_string(view, "window.__fs = 'pending'; document.addEventListener('click', () => document.querySelector('video').requestFullscreen()"
                          ".then(() => { window.__fs = 'ok'; }, e => { window.__fs = 'rejected: ' + e.message; }), {once: true}); 'armed'");
        devtools_click(view, 300, 300);
        std::optional<std::string> state;
        pump_until([&]() {
            state = eval_string(view, "String(window.__fs)");
            if (state && *state != "pending") {
                return true;
            }
            pump_for(100ms);
            return false;
        }, 5s);
        return state.value_or("no answer");
    };
    std::string result = "click: " + attempt();
    if (result != "click: ok") {
        // The runtime only grants fullscreen to a document that has focus (document.hasFocus()); a real click
        // focuses the WebView, a DevTools click does not. This steals focus, once.
        window.focus();
        view.focus();
        pump_for(500ms);
        result += "; after focusing the WebView: " + attempt();
    }
    return result;
}

std::string monitor_text(HWND hwnd, bool& covers) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    covers = EqualRect(&wr, &mi.rcMonitor) != FALSE;
    return "window " + std::to_string(wr.right - wr.left) + "x" + std::to_string(wr.bottom - wr.top) + ", monitor " +
           std::to_string(mi.rcMonitor.right - mi.rcMonitor.left) + "x" + std::to_string(mi.rcMonitor.bottom - mi.rcMonitor.top);
}

/// What users do with a streaming app: fullscreen over the whole screen, then leave it running in the
/// background (minimized, or behind another window), and share it in Discord/Teams (WGC captures in measure()).
void run_user(boson::core::HostingMode mode, const std::filesystem::path& root, const std::filesystem::path& out, Report& report,
              std::chrono::milliseconds window_len) {
    const std::string mode_name = mode == boson::core::HostingMode::Composition ? "composition" : "windowed";
    std::cout << "== fullscreen / background / sharing (" << mode_name << ") ==\n";
    boson::core::EnvironmentConfig cfg;
    cfg.user_data_folder = (root / (L"udf-user-" + boson::core::utf8_to_wide(mode_name))).wstring();
    auto rig = make_rig(mode, cfg, L"Boson DRM spike (fullscreen/background)");
    boson::core::Window& window = *rig->window;
    boson::core::WebView& a = create_view(*rig, "boson://app/blank.html");
    // What an app does for element.requestFullscreen(): make its own window borderless over the whole monitor.
    a.events().on_fullscreen_changed = [&window](bool fullscreen) { window.set_fullscreen(fullscreen); };

    const auto background = [&](const std::string& what, const std::function<void()>& enter, const std::function<void()>& leave,
                                bool capture_while_away) {
        const auto b0 = sample(a);
        enter();
        pump_for(4s);
        std::string captured;
        if (capture_while_away) {
            std::string failure;
            const Image shot = grab_wgc(window.hwnd(), false, failure);
            captured = ", WGC window capture while away " + fmt(visible_percent(shot, a.bounds())) + " % visible" + (failure.empty() ? "" : " (" + failure + ")");
        }
        pump_for(4s);
        const auto b1 = sample(a);
        const bool playing_audio = a.is_playing_audio().value_or(false);
        const bool iconic = IsIconic(window.hwnd()) != FALSE;
        leave();
        pump_for(2s);
        std::string note = what + ": ";
        if (b0 && b1) {
            const double advanced = b1->t - b0->t;
            note += std::string(advanced > 8.0 * 0.6 ? "KEPT PLAYING" : "STALLED") + ", advanced " + fmt(advanced) + " s in 8 s, state " + b1->drm.state +
                    (b1->paused ? " (paused)" : "") + ", frames +" + std::to_string(b1->total_frames - std::min(b1->total_frames, b0->total_frames)) +
                    ", audio bytes " + (b1->audio_bytes > b0->audio_bytes ? "growing" : "flat") + ", IsDocumentPlayingAudio " +
                    (playing_audio ? "true" : "false") + (iconic ? ", window was minimized" : "") + captured;
        } else {
            note += "no sample";
        }
        std::cout << "    " << note << '\n';
        return note;
    };

    const auto wanted = [](std::string_view what) { return g_user_filter.empty() || g_user_filter.find(what) != std::string::npos; };
    for (const Stream& stream : {playready_sl3000(), widevine_l3()}) {
        const std::string tag = stream.key_system.find("widevine") != std::string::npos ? "Widevine" : "SL3000";
        if (!wanted(tag)) {
            continue;
        }
        // Fullscreen over the entire screen, the way a streaming site does it.
        if (wanted("fullscreen")) {
            navigate_and_wait(a, player_url(stream));
            const std::string reply = enter_fullscreen(a, window);
            pump_for(2s);
            bool covers = false;
            const std::string geometry = monitor_text(window.hwnd(), covers);
            const auto state = eval_string(a, "(document.fullscreenElement ? document.fullscreenElement.tagName : 'none') + ' ' + innerWidth + 'x' + innerHeight");
            const std::string note = "requestFullscreen via " + reply +
                                     "; fullscreenElement " + state.value_or("?") + "; " + geometry + (covers ? " (covers the whole monitor)" : " (NOT the whole monitor)") +
                                     "; ContainsFullScreenElement " + (a.contains_fullscreen_element().value_or(false) ? "true" : "false");
            add_case(report, measure(mode_name, "13 fullscreen, whole monitor (" + tag + ")", stream, window, a, window_len, out, note));
            devtools_eval(a, "document.exitFullscreen().then(() => 'ok', e => 'rejected: ' + e.message)");
            pump_for(2s);
            add_case(report, measure(mode_name, "13 after leaving fullscreen (" + tag + ")", stream, window, a, window_len, out,
                                     std::string("window back to normal: ") + (window.is_fullscreen() ? "no" : "yes")));
        }

        // Background: minimized (WebViewHost hides the controller, IsVisible = false), then restored.
        if (wanted("minimized")) {
            navigate_and_wait(a, player_url(stream));
            const std::string minimized = background(
                "while minimized 8 s", [&]() { ShowWindow(window.hwnd(), SW_SHOWMINNOACTIVE); }, [&]() { ShowWindow(window.hwnd(), SW_SHOWNOACTIVATE); },
                false);
            add_case(report, measure(mode_name, "14 minimized, then restored (" + tag + ")", stream, window, a, window_len, out, minimized));

            // The same, but the controller stays IsVisible = true while the window is minimized.
            navigate_and_wait(a, player_url(stream));
            const std::string kept = background(
                "while minimized 8 s, controller kept visible",
                [&]() {
                    a.set_keep_visible_when_minimized(true);
                    ShowWindow(window.hwnd(), SW_SHOWMINNOACTIVE);
                },
                [&]() {
                    ShowWindow(window.hwnd(), SW_SHOWNOACTIVATE);
                    a.set_keep_visible_when_minimized(false);
                },
                false);
            add_case(report, measure(mode_name, "14b minimized with keep_visible_when_minimized (" + tag + ")", stream, window, a, window_len, out, kept));
        }

        // Background: the app hides the WebView (IsVisible = false), e.g. another tab is shown.
        if (wanted("hidden")) {
            navigate_and_wait(a, player_url(stream));
            const std::string hidden = background(
                "while the WebView is hidden 8 s", [&]() { a.set_visible(false); }, [&]() { a.set_visible(true); }, false);
            add_case(report, measure(mode_name, "16 WebView hidden, then shown (" + tag + ")", stream, window, a, window_len, out, hidden));
        }

        // Background: fully covered by another (opaque) window, then uncovered.
        if (!wanted("covered")) {
            continue;
        }
        navigate_and_wait(a, player_url(stream));
        HWND cover = nullptr;
        const std::string covered = background(
            "while covered by another window 8 s",
            [&]() {
                RECT wr{};
                GetWindowRect(window.hwnd(), &wr);
                cover = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Boson DRM spike cover", WS_POPUP | WS_VISIBLE, wr.left, wr.top,
                                        wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (cover != nullptr) {
                    SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            },
            [&]() {
                if (cover != nullptr) {
                    DestroyWindow(cover);
                    cover = nullptr;
                }
            },
            true);
        add_case(report, measure(mode_name, "15 covered by another window, then uncovered (" + tag + ")", stream, window, a, window_len, out, covered));
    }
    a.events().on_fullscreen_changed = nullptr;
    shutdown_rig(rig);
}

/// --fs-debug: tries the ways a page can enter element fullscreen and prints what the runtime answers.
void run_fullscreen_debug(const std::filesystem::path& root) {
    boson::core::EnvironmentConfig cfg;
    cfg.user_data_folder = (root / L"udf-fs").wstring();
    auto rig = make_rig(boson::core::HostingMode::Windowed, cfg, L"Boson DRM spike (fullscreen debug)");
    boson::core::WebView& a = create_view(*rig, "boson://app/blank.html");
    boson::core::Window& window = *rig->window;
    a.events().on_fullscreen_changed = [&window](bool fullscreen) {
        std::cout << "    ContainsFullScreenElementChanged -> " << fullscreen << '\n';
        window.set_fullscreen(fullscreen);
    };
    const auto report_state = [&](const std::string& what, const std::string& reply) {
        pump_for(1s);
        std::cout << "  " << what << ": " << reply.substr(0, 200) << " | fullscreenElement="
                  << eval_string(a, "String(document.fullscreenElement && document.fullscreenElement.tagName)").value_or("?")
                  << " enabled=" << eval_string(a, "String(document.fullscreenEnabled)").value_or("?")
                  << " hasFocus=" << eval_string(a, "String(document.hasFocus())").value_or("?") << " window.is_fullscreen=" << window.is_fullscreen() << '\n';
        devtools_eval(a, "document.fullscreenElement ? document.exitFullscreen().then(() => 'exited') : 'none'");
        pump_for(1s);
    };
    report_state("blank page, documentElement, Runtime userGesture",
                 devtools_eval(a, "document.documentElement.requestFullscreen().then(() => 'ok', e => 'rejected: ' + e.name + ' ' + e.message)"));
    a.focus();
    pump_for(500ms);
    report_state("after WebView::focus, documentElement, Runtime userGesture",
                 devtools_eval(a, "document.documentElement.requestFullscreen().then(() => 'ok', e => 'rejected: ' + e.name + ' ' + e.message)"));
    eval_string(a, "window.__fs = 'pending'; document.addEventListener('click', () => document.documentElement.requestFullscreen()"
                   ".then(() => { window.__fs = 'ok'; }, e => { window.__fs = 'rejected: ' + e.name + ' ' + e.message; }), {once: true}); 'armed'");
    devtools_click(a, 200, 200);
    pump_for(1s);
    report_state("click handler, documentElement", eval_string(a, "String(window.__fs) + ' activation=' + navigator.userActivation.hasBeenActive").value_or("?"));
    const Stream wv = widevine_l3();
    navigate_and_wait(a, player_url(wv));
    report_state("player page, video, Runtime userGesture",
                 devtools_eval(a, "document.querySelector('video').requestFullscreen().then(() => 'ok', e => 'rejected: ' + e.name + ' ' + e.message)"));
    report_state("player page, documentElement, Runtime userGesture",
                 devtools_eval(a, "document.documentElement.requestFullscreen().then(() => 'ok', e => 'rejected: ' + e.name + ' ' + e.message)"));
    a.events().on_fullscreen_changed = nullptr;
    shutdown_rig(rig);
}

void run_flags(const std::filesystem::path& root, const std::filesystem::path& out, Report& report, std::chrono::milliseconds window_len) {
    std::cout << "== EnvironmentConfig flags ==\n";
    struct FlagCase {
        std::string name;
        std::vector<std::wstring> disable;
        std::vector<std::wstring> enable;
        std::vector<std::wstring> args;
        bool play = false;
    };
    const std::vector<FlagCase> cases = {
        {.name = "disable HardwareSecureDecryption", .disable = {L"HardwareSecureDecryption"}},
        {.name = "disable HardwareSecureDecryptionExperiment", .disable = {L"HardwareSecureDecryptionExperiment"}},
        {.name = "enable HardwareSecureDecryptionExperiment", .enable = {L"HardwareSecureDecryptionExperiment"}},
        {.name = "disable EdgePlayReadyDrmWin10", .disable = {L"EdgePlayReadyDrmWin10"}},
        {.name = "disable msPlayReadyWin10", .disable = {L"msPlayReadyWin10"}, .play = true},
        {.name = "enable msDisablePlayReadyHardwareDrm", .enable = {L"msDisablePlayReadyHardwareDrm"}, .play = true},
        {.name = "enable msPlayReadyCbcsEncryptionScheme", .enable = {L"msPlayReadyCbcsEncryptionScheme"}},
        {.name = "disable PlatformEncryptedDolbyVision", .disable = {L"PlatformEncryptedDolbyVision"}},
        {.name = "enable PlatformEncryptedDolbyVision", .enable = {L"PlatformEncryptedDolbyVision"}},
        // Screen-share candidates: without DirectComposition video overlays the video is composited into the
        // main swap chain instead of a protected/display-only overlay swap chain.
        {.name = "--disable-direct-composition", .args = {L"--disable-direct-composition"}, .play = true},
        {.name = "disable msPlayReadyWin10 + --disable-direct-composition",
         .disable = {L"msPlayReadyWin10"},
         .args = {L"--disable-direct-composition"},
         .play = true},
        {.name = "disable msPlayReadyWin10 + DirectCompositionSoftwareOverlays", .disable = {L"msPlayReadyWin10", L"DirectCompositionSoftwareOverlays"}, .play = true},
        {.name = "--disable-gpu", .args = {L"--disable-gpu"}, .play = true},
        {.name = "--disable-gpu + msDisablePlayReadyHardwareDrm", .enable = {L"msDisablePlayReadyHardwareDrm"}, .args = {L"--disable-gpu"}, .play = true},
    };
    boson::webview::DrmProbeOptions options;
    options.video_codecs.push_back({.name = "dvh1", .content_type = R"(video/mp4; codecs="dvh1.05.06")"});
    options.video_codecs.push_back({.name = "hvc1", .content_type = R"(video/mp4; codecs="hvc1.2.4.L153.B0")"});
    int index = 0;
    {
        boson::core::EnvironmentConfig cfg;
        cfg.user_data_folder = (root / L"udf-flags-default").wstring();
        auto rig = make_rig(boson::core::HostingMode::Windowed, cfg, L"Boson DRM spike (flags)");
        boson::core::WebView& v = create_view(*rig, "boson://app/blank.html");
        pump_for(1s);
        report.probes.push_back(probe(v, "flags: default (+dvh1/hvc1 codecs)", options));
        shutdown_rig(rig);
    }
    for (const auto& c : cases) {
        boson::core::EnvironmentConfig cfg;
        cfg.user_data_folder = (root / (L"udf-flags-" + std::to_wstring(index++))).wstring();
        cfg.disable_features = c.disable;
        cfg.enable_features = c.enable;
        cfg.additional_browser_arguments = c.args;
        auto rig = make_rig(boson::core::HostingMode::Windowed, cfg, L"Boson DRM spike (flags)");
        boson::core::WebView& v = create_view(*rig, "boson://app/blank.html");
        pump_for(1s);
        report.probes.push_back(probe(v, "flags: " + c.name + " (+dvh1/hvc1 codecs)", options));
        if (c.play) {
            const Stream sl3000 = playready_sl3000();
            navigate_and_wait(v, player_url(sl3000));
            add_case(report, measure("windowed", "12 " + c.name + ": SL3000", sl3000, *rig->window, v, window_len, out));
            const Stream sl2000 = playready_sl2000_key_system(kPlayReadyLicenseSl150, "PlayReady SL2000 key system, sl:150 licence");
            navigate_and_wait(v, player_url(sl2000));
            add_case(report, measure("windowed", "12 " + c.name + ": SL2000 sl:150", sl2000, *rig->window, v, window_len, out));
            const Stream wv = widevine_l3();
            navigate_and_wait(v, player_url(wv));
            add_case(report, measure("windowed", "12 " + c.name + ": Widevine", wv, *rig->window, v, window_len, out));
        }
        shutdown_rig(rig);
    }
}

void write_markdown(const Report& report, const std::filesystem::path& path) {
    std::ofstream md(path, std::ios::binary | std::ios::trunc);
    md << "OS build " << report.os_build << ", GPU " << report.gpu << ", WebView2 runtime " << report.runtime << "\n\n";
    md << "| Hosting | Case | Stream | Key system (robustness) | Plays | Resolution | Frames decoded / dropped / presented | "
          "Visible % DXGI / PrintWindow / CapturePreview / WGC window / WGC monitor | Result | Notes |\n";
    md << "|---|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& r : report.cases) {
        std::string notes = r.note;
        if (!r.error.empty()) {
            notes += (notes.empty() ? "" : "; ") + r.error.substr(0, 160);
        }
        for (auto& ch : notes) {
            if (ch == '|') {
                ch = '/';
            }
        }
        md << "| " << r.mode << " r" << r.round << " | " << r.name << " | " << r.stream << " | " << (r.key_system.empty() ? "-" : r.key_system) << " ("
           << (r.robustness.empty() ? "-" : r.robustness) << ") | " << (r.plays ? "yes" : "no") << " (+" << fmt(r.advanced_s) << " s) | "
           << r.width << "x" << r.height << " | " << r.frames << " / " << r.dropped << " / " << r.presented << " | " << fmt(r.screen_visible)
           << " / " << fmt(r.print_window_visible) << " / " << fmt(r.preview_visible) << " / " << fmt(r.wgc_window_visible) << " / "
           << fmt(r.wgc_monitor_visible) << " | " << r.verdict << " | " << notes << " |\n";
    }
    // Aggregate over rounds: how often each case played.
    md << "\n| Hosting | Case | Rounds played | Max visible % (DXGI / PrintWindow / CapturePreview / WGC window / WGC monitor) |\n|---|---|---|---|\n";
    std::vector<std::string> keys;
    for (const auto& r : report.cases) {
        const std::string key = r.mode + " | " + r.name;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }
    for (const auto& key : keys) {
        int total = 0;
        int played = 0;
        double screen = -1.0;
        double printed = -1.0;
        double preview = -1.0;
        double wgc_window = -1.0;
        double wgc_monitor = -1.0;
        for (const auto& r : report.cases) {
            if (r.mode + " | " + r.name != key) {
                continue;
            }
            ++total;
            played += r.plays ? 1 : 0;
            if (!r.occluded) {
                screen = std::max(screen, r.screen_visible);
            }
            printed = std::max(printed, r.print_window_visible);
            preview = std::max(preview, r.preview_visible);
            wgc_window = std::max(wgc_window, r.wgc_window_visible);
            if (!r.occluded) {
                wgc_monitor = std::max(wgc_monitor, r.wgc_monitor_visible);
            }
        }
        md << "| " << key << " | " << played << "/" << total << " | " << fmt(screen) << " / " << fmt(printed) << " / " << fmt(preview) << " / "
           << fmt(wgc_window) << " / " << fmt(wgc_monitor) << " |\n";
    }
    md << "\n| Probe context | Protection | Best | Supported | Not supported |\n|---|---|---|---|---|\n";
    for (const auto& p : report.probes) {
        std::string supported;
        for (const auto& s : p.supported) {
            supported += (supported.empty() ? "" : "<br>") + s;
        }
        std::string unsupported;
        for (const auto& s : p.unsupported) {
            unsupported += (unsupported.empty() ? "" : ", ") + s;
        }
        md << "| " << p.context << " | " << p.protection << " | " << p.best << " | " << supported << " | " << unsupported << " |\n";
    }
}

} // namespace drm_spike

int main(int argc, char** argv) {
    using namespace drm_spike;
    std::filesystem::path out = std::filesystem::current_path() / "drm_spike_results";
    bool windowed = true;
    bool composition = true;
    bool flags = true;
    bool matrix = true;
    bool fs_debug = false;
    bool user = true;
    int seconds = 5;
    int rounds = 1;
    std::vector<std::wstring> custom_disable;
    std::vector<std::wstring> custom_enable;
    std::vector<std::wstring> custom_args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (arg == "--modes" && i + 1 < argc) {
            const std::string_view modes = argv[++i];
            windowed = modes.find("windowed") != std::string_view::npos;
            composition = modes.find("composition") != std::string_view::npos;
        } else if ((arg == "--mpd" || arg == "--lic" || arg == "--ks" || arg == "--vr" || arg == "--axinom-token") && i + 1 < argc) {
            if (!g_custom) {
                g_custom = Stream{.label = "custom"};
            }
            const std::string value = argv[++i];
            if (arg == "--mpd") g_custom->mpd = value;
            if (arg == "--lic") g_custom->license = value;
            if (arg == "--ks") g_custom->key_system = value;
            if (arg == "--vr") g_custom->robustness = value;
            if (arg == "--axinom-token") {
                g_custom->header_name = "X-AxDRM-Message";
                g_custom->header_value = value == "default" ? std::string(kAxinomToken) : value;
            }
        } else if ((arg == "--disable" || arg == "--enable" || arg == "--arg") && i + 1 < argc) {
            const std::wstring value = boson::core::utf8_to_wide(argv[++i]);
            (arg == "--disable" ? custom_disable : arg == "--enable" ? custom_enable : custom_args).push_back(value);
        } else if (arg == "--start" && i + 1 < argc) {
            g_start = argv[++i];
        } else if (arg == "--maxh" && i + 1 < argc) {
            g_maxh = argv[++i];
        } else if (arg == "--repeat" && i + 1 < argc) {
            g_repeat = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--udf" && i + 1 < argc) {
            g_udf = boson::core::utf8_to_wide(argv[++i]);
        } else if (arg == "--blank-between") {
            g_blank_between = true;
        } else if (arg == "--rounds" && i + 1 < argc) {
            rounds = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--keep-cache") {
            g_keep_cache = true;
        } else if (arg == "--quick") {
            g_quick = true;
        } else if (arg == "--skip-flags") {
            flags = false;
        } else if (arg == "--skip-matrix") {
            matrix = false;
        } else if (arg == "--skip-user") {
            user = false;
        } else if (arg == "--fs-debug") {
            fs_debug = true;
        } else if (arg == "--user" && i + 1 < argc) {
            g_user_filter = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::max(2, std::atoi(argv[++i]));
        } else {
            std::cerr << "usage: boson_drm_spike [--out DIR] [--modes windowed,composition] [--rounds N] [--seconds N] [--skip-matrix] [--skip-user] [--skip-flags]\n";
            return 2;
        }
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    Report report;
    {
        LPWSTR version = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cerr << "no WebView2 runtime installed\n";
            CoUninitialize();
            return 1;
        }
        report.runtime = to_utf8(version);
        CoTaskMemFree(version);
    }
    report.os_build = os_build();
    g_report = &report;
    g_results_md = out / "results.md";
    report.gpu = gpu_description();
    std::cout << "OS build " << report.os_build << ", GPU " << report.gpu << ", WebView2 " << report.runtime << '\n';

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    // Under %LOCALAPPDATA% rather than %TEMP%: the PlayReady CDM keeps its store in the user data folder, and a
    // standalone Edge profile under a redirected TEMP failed with MSPR_E_UNABLE_TO_OPEN_DATA_STORE.
    wchar_t local[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) {
        GetTempPathW(MAX_PATH, local);
    }
    const std::filesystem::path root = std::filesystem::path(local) / (L"BosonDrmSpike-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root, ec);
    const auto window_len = std::chrono::milliseconds{seconds * 1000};

    if (fs_debug) {
        run_fullscreen_debug(root);
        windowed = composition = flags = user = matrix = false;
    }
    if (g_custom) {
        g_custom_composition = composition && !windowed;
        run_custom(root, out, report, window_len, custom_disable, custom_enable, custom_args);
        windowed = composition = flags = user = false;
    }
    for (int round = 1; matrix && round <= rounds; ++round) {
        std::cout << "#### round " << round << "/" << rounds << '\n';
        g_round = round;
        if (windowed) {
            run_mode(boson::core::HostingMode::Windowed, root / ("r" + std::to_string(round)), out, report, window_len);
        }
        if (composition) {
            run_mode(boson::core::HostingMode::Composition, root / ("r" + std::to_string(round)), out, report, window_len);
        }
    }
    if (user) {
        g_round = 1;
        if (windowed) {
            run_user(boson::core::HostingMode::Windowed, root, out, report, window_len);
        }
        if (composition) {
            run_user(boson::core::HostingMode::Composition, root, out, report, window_len);
        }
    }
    if (flags) {
        run_flags(root, out, report, window_len);
    }

    write_markdown(report, out / "results.md");
    std::string json;
    if (!glz::write<glz::opts{.prettify = true}>(report, json)) {
        std::ofstream(out / "results.json", std::ios::binary | std::ios::trunc) << json;
    }
    std::cout << "results: " << (out / "results.md").string() << '\n';
    for (int i = 0; i < 20; ++i) {
        std::filesystem::remove_all(root, ec);
        if (!std::filesystem::exists(root)) {
            break;
        }
        Sleep(250);
    }
    CoUninitialize();
    return 0;
}
