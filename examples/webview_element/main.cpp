// boson_webview_element: two <boson-webview> panes (separate partitions) in a resizable split, a URL bar driving
// the focused pane, an event log, popups adopted into an overlay pane, and an optional Chrome DevTools Protocol
// endpoint (--cdp) that Playwright / Puppeteer can connectOverCDP to.
//
//   boson_webview_element.exe [--cdp] [url-left] [url-right]
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boson/boson.hpp>

namespace {

// frontend/src/webview_element.ts bundled by bun at build time (CMake), embedded in the binary.
#if defined(BOSON_USE_CPP26_EMBED)
constexpr std::uint8_t kElementJs[] = {
#embed "webview_element.js"
};
#else
#include "webview_element_js.hpp" // kElementJs: the same bytes as a constexpr array (cmake/EmbedFile.cmake)
#endif

constexpr std::string_view kHostHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Boson webview element</title>
<style>
  :root { color-scheme: dark; font: 13px system-ui, sans-serif; }
  html, body { margin: 0; height: 100%; background: #1b1d22; color: #ddd; overflow: hidden; }
  #bar { display: flex; gap: 6px; padding: 6px; align-items: center; background: #24272e; }
  #bar input { flex: 1; padding: 4px 8px; background: #15171b; color: #eee; border: 1px solid #3a3f4a; border-radius: 4px; }
  #bar button { background: #323743; color: #eee; border: 0; padding: 4px 10px; border-radius: 4px; }
  #split { position: absolute; top: 40px; bottom: 120px; left: 0; right: 0; display: flex; }
  #split boson-webview { display: block; height: 100%; min-width: 80px; }
  #left { width: 50%; } #right { flex: 1; }
  #handle { width: 6px; cursor: col-resize; background: #2d313a; }
  boson-webview.focused { outline: 2px solid #4f8cff; outline-offset: -2px; }
  #log { position: absolute; bottom: 0; left: 0; right: 0; height: 120px; overflow: auto; margin: 0; padding: 4px 8px;
         background: #111316; font: 11px ui-monospace, monospace; color: #9ab; }
  #popup { position: absolute; right: 24px; bottom: 140px; width: 420px; height: 300px; display: none;
           border: 1px solid #4f8cff; background: #000; }
  #popup.open { display: block; }
  #popup boson-webview { display: block; width: 100%; height: calc(100% - 24px); }
  #popup header { height: 24px; display: flex; justify-content: space-between; align-items: center; padding: 0 6px; background: #24272e; }
</style></head>
<body>
  <div id="bar">
    <button id="back">&#8592;</button><button id="fwd">&#8594;</button><button id="reload">&#8635;</button>
    <input id="url" spellcheck="false">
    <button id="devtools">DevTools</button><button id="shot">Capture</button><span id="cdp"></span>
  </div>
  <div id="split">
    <boson-webview id="left" partition="persist:left" allowpopups console></boson-webview>
    <div id="handle"></div>
    <boson-webview id="right" partition="right" allowpopups console></boson-webview>
  </div>
  <div id="popup"><header><span id="popup-title">popup</span><button id="popup-close">x</button></header></div>
  <pre id="log"></pre>
  <script src="webview_element.js"></script>
  <script>
    const $ = (id) => document.getElementById(id);
    const log = (text) => { $('log').textContent += text + '\n'; $('log').scrollTop = 1e9; };
    const params = new URLSearchParams(location.search);
    let focused = $('left');
    const panes = [$('left'), $('right')];
    $('left').src = params.get('left') || 'https://example.com/';
    $('right').src = params.get('right') || 'https://www.wikipedia.org/';
    const watch = (wv, name) => {
      for (const type of ['did-navigate', 'did-fail-load', 'page-title-updated', 'new-window', 'console-message',
                          'ipc-message', 'download-started', 'download-done', 'crashed', 'permission-request']) {
        wv.addEventListener(type, (e) => {
          const d = e.detail;
          log(name + ' ' + type + ' ' + (d.url || d.title || d.message || d.channel || d.permission || ''));
          if (type === 'did-navigate' && wv === focused) $('url').value = d.url;
          if (type === 'new-window' && d.disposition === 'guest') openPopup(d.new_guest_id, d.url);
        });
      }
      wv.addEventListener('guest-focus', () => setFocused(wv));
      wv.addEventListener('focus', () => setFocused(wv));
    };
    const setFocused = (wv) => {
      focused = wv;
      for (const p of document.querySelectorAll('boson-webview')) p.classList.toggle('focused', p === wv);
      $('url').value = wv.getURL();
    };
    panes.forEach((wv, i) => watch(wv, i === 0 ? 'L' : 'R'));
    setFocused($('left'));
    $('url').addEventListener('keydown', (e) => {
      if (e.key !== 'Enter') return;
      let url = $('url').value.trim();
      if (!/^[a-z]+:/i.test(url)) url = 'https://' + url;
      focused.loadURL(url);
    });
    $('back').onclick = () => focused.goBack();
    $('fwd').onclick = () => focused.goForward();
    $('reload').onclick = () => focused.reload();
    $('devtools').onclick = () => focused.openDevTools();
    $('shot').onclick = async () => {
      const img = await focused.capturePage();
      log('captured ' + img.size + ' bytes of ' + img.mimeType);
    };
    // Accelerators pressed inside a guest bubble here as keydown events.
    document.addEventListener('keydown', (e) => {
      if (e.ctrlKey && e.key === 'l') { e.preventDefault(); $('url').focus(); $('url').select(); }
      if (e.key === 'F5') { e.preventDefault(); focused.reload(); }
    });
    // Drag the handle: the native views follow the layout every animation frame.
    $('handle').addEventListener('pointerdown', (e) => {
      $('handle').setPointerCapture(e.pointerId);
      const move = (m) => { $('left').style.width = Math.max(80, Math.min(innerWidth - 86, m.clientX)) + 'px'; };
      $('handle').addEventListener('pointermove', move);
      $('handle').addEventListener('pointerup', () => $('handle').removeEventListener('pointermove', move), { once: true });
    });
    function openPopup(id, url) {
      $('popup').querySelector('boson-webview')?.remove();
      const wv = document.createElement('boson-webview');
      wv.setAttribute('guestinstance', String(id));
      watch(wv, 'P');
      $('popup').appendChild(wv);
      $('popup').classList.add('open');
      $('popup-title').textContent = url;
    }
    $('popup-close').onclick = () => { $('popup').querySelector('boson-webview')?.remove(); $('popup').classList.remove('open'); };
    // Guest.cdp_endpoint over the raw bridge protocol (the bundled module keeps its boson client private).
    const CDP_CALL_ID = 2147480000;
    chrome.webview.addEventListener('message', (e) => {
      const r = e.data;
      if (r && r.id === CDP_CALL_ID && r.data && r.data.enabled) {
        $('cdp').textContent = 'CDP ' + r.data.http;
        log('chromium.connectOverCDP("' + r.data.http + '")');
      }
    });
    chrome.webview.postMessage({ id: CDP_CALL_ID, method: 'Guest.cdp_endpoint', params: {} });
  </script>
</body></html>)HTML";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/index.html") {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(kHostHtml.data()), kHostHtml.size());
    }
    if (path == "/webview_element.js") {
        return std::as_bytes(std::span<const std::uint8_t>(std::begin(kElementJs), std::end(kElementJs)));
    }
    return std::nullopt;
}

std::string query_value(std::string_view value) {
    std::string out;
    for (const char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            constexpr char kHex[] = "0123456789ABCDEF";
            const auto b = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 15]);
        }
    }
    return out;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev_instance*/, PWSTR /*cmd_line*/, int show_cmd) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }
    bool enable_cdp = false;
    std::vector<std::string> urls;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv != nullptr && i < argc; ++i) {
            const std::wstring_view arg(argv[i]);
            if (arg == L"--cdp") {
                enable_cdp = true;
            } else {
                urls.push_back(boson::core::wide_to_utf8(arg));
            }
        }
        if (argv != nullptr) {
            LocalFree(argv);
        }
    }
    std::string start = "boson://app/index.html";
    if (!urls.empty()) {
        start += "?left=" + query_value(urls[0]);
        if (urls.size() > 1) {
            start += "&right=" + query_value(urls[1]);
        }
    }

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson - <boson-webview>";
    window_cfg.width = 1280;
    window_cfg.height = 860;
    window_cfg.dark_mode = true;
    boson::core::Window window(std::move(window_cfg));
    if (!window.create(instance, show_cmd)) {
        CoUninitialize();
        return 1;
    }

    int exit_code = 0;
    {
        boson::exec::Scheduler scheduler;
        boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
        window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
        boson::exec::TaskRunner runner(scheduler, marshal);

        const auto environment = boson::core::Environment::create(boson::core::EnvironmentConfig{});
        boson::core::WebViewHost host(window, environment);
        boson::core::WebViewConfig host_cfg;
        host_cfg.initial_url = start;
        boson::core::WebView& page = host.create(std::move(host_cfg), boson::core::WebViewCallbacks{.asset_lookup = &lookup});

        boson::webview::GuestManager guests(host);
        boson::ipc::GuestController guest_controller(guests, marshal);
        boson::ipc::Router<boson::ipc::GuestController> router(guest_controller);
        boson::ipc::Bridge bridge(runner, marshal, router);
        connect_events(guest_controller, bridge);
        bridge.attach(page);

        std::unique_ptr<boson::webview::CdpServer> cdp;
        std::unique_ptr<boson::webview::CdpGuestBridge> cdp_guests;
        if (enable_cdp) {
            cdp = std::make_unique<boson::webview::CdpServer>();
            if (cdp->start()) {
                // Targets created by the client (context.newPage) open as guests owned by the page.
                cdp_guests = std::make_unique<boson::webview::CdpGuestBridge>(
                    *cdp, guests, boson::webview::CdpGuestBridgeOptions{.owner_id = page.id(), .defaults = {}, .expose_existing = true});
                guest_controller.set_cdp_server(cdp.get());
                const std::string line = "CDP endpoint: " + cdp->http_endpoint() + "\n";
                OutputDebugStringA(line.c_str());
            }
        }

        bool quitting = false;
        window.on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
            if (quitting) {
                return;
            }
            quitting = true;
            e.cancel = true;
            window.hide();
            cdp_guests.reset();
            if (cdp) {
                cdp->stop();
            }
            disconnect_events(guest_controller);
            guests.destroy_all();
            boson::app::quit({environment});
        });
        exit_code = boson::core::Window::run_message_loop();
        disconnect_events(guest_controller);
    }
    CoUninitialize();
    return exit_code;
}
