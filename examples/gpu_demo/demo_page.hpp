#pragma once

#include <string_view>

namespace boson::gpu_demo {

/// Served as boson://app/index.html. The <boson-surface> element runtime is injected by GpuController.
inline constexpr std::string_view kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Boson GPU demo</title>
<style>
  html, body { margin: 0; height: 100%; background: transparent; color: #e9e9f2; overflow: hidden;
               font: 14px/1.45 "Segoe UI Variable Text", "Segoe UI", sans-serif; }
  #terminal { position: fixed; inset: 0; }
  .panel { position: fixed; top: 24px; right: 24px; bottom: 24px; width: 400px; box-sizing: border-box; padding: 18px 20px;
           background: rgba(22, 24, 36, 0.72); border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 14px;
           box-shadow: 0 12px 40px rgba(0, 0, 0, 0.45); overflow: auto; }
  h1 { font-size: 18px; margin: 0 0 6px; }
  p { margin: 6px 0 12px; color: #c4c6d6; }
  pre { background: rgba(0, 0, 0, 0.35); border-radius: 8px; padding: 10px; font: 12px/1.4 "Cascadia Mono", Consolas, monospace;
        white-space: pre-wrap; margin: 0 0 12px; }
  .card { position: relative; height: 210px; overflow: auto; border-radius: 10px; border: 1px solid rgba(255, 255, 255, 0.18);
          margin: 8px 0 12px; }
  .card boson-surface { height: 320px; }
  .card .spacer { height: 160px; display: flex; align-items: center; justify-content: center; color: #9aa; }
  button { background: #3b5bdb; color: white; border: 0; border-radius: 8px; padding: 7px 14px; font: inherit; cursor: pointer; }
  button:hover { background: #4c6ef5; }
  .badge { position: fixed; left: 24px; bottom: 24px; width: 300px; height: 64px; }
  .pulse { display: inline-block; width: 10px; height: 10px; border-radius: 50%; background: #51cf66; margin-right: 6px;
           animation: pulse 1.2s ease-in-out infinite; }
  @keyframes pulse { 50% { opacity: 0.2; transform: scale(0.7); } }
</style>
</head>
<body>
  <boson-surface id="terminal" content="terminal" layer="below"></boson-surface>
  <boson-surface class="badge" content="badge" layer="above"></boson-surface>
  <div class="panel">
    <h1><span class="pulse"></span>Boson native GPU surfaces</h1>
    <p>The text grid behind this panel is drawn by C++ (Direct3D + DirectWrite glyph atlas) into a composition swap
       chain below the WebView. This panel is ordinary HTML with a semi-transparent background.</p>
    <pre id="info">Gpu.info()...</pre>
    <button id="toggle">Toggle Vulkan surface</button>
    <div class="card" id="card">
      <boson-surface id="vulkan" content="vulkan" z-index="1"></boson-surface>
      <div class="spacer">scroll: the native surface is clipped to this box</div>
    </div>
    <p id="status"></p>
  </div>
<script>
(() => {
  const gpu = window.__bosonGpu;
  const info = document.getElementById('info');
  const status = document.getElementById('status');
  if (!gpu) { info.textContent = 'Not running inside Boson (window.__bosonGpu missing).'; return; }
  gpu.call('Gpu.info').then((i) => {
    info.textContent = [
      'backend      ' + i.backend + '  FL ' + i.feature_level,
      'adapter      ' + i.adapter.name,
      'vram         ' + i.adapter.dedicated_video_memory_mb + ' MiB',
      'hosting      ' + i.hosting,
      'tearing      ' + i.tearing_supported + '   compositor clock ' + i.compositor_clock,
      'vulkan       ' + (i.vulkan_available ? i.vulkan_version : 'not installed'),
      'contents     ' + i.contents.join(', '),
    ].join('\n');
  }).catch((e) => { info.textContent = String(e); });
  window.chrome.webview.addEventListener('message', (e) => {
    const m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
    if (m && m.type === 'event' && m.event === 'gpu:device_lost') {
      status.textContent = 'GPU device lost (' + m.data.reason + '), recovered: ' + m.data.recovered;
    }
  });
  const card = document.getElementById('card');
  let vk = document.getElementById('vulkan');
  document.getElementById('toggle').addEventListener('click', () => {
    if (vk.isConnected) { vk.remove(); }
    else { card.prepend(vk); }
  });
  document.addEventListener('boson-surface-error', (e) => { status.textContent = 'surface error: ' + e.detail; });
})();
</script>
</body>
</html>
)HTML";

} // namespace boson::gpu_demo
