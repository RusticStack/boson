#pragma once

#include <string_view>

namespace boson::browser {

// The browser chrome (tab strip, URL bar, window buttons, settings modal). Served from memory at
// boson://app/shell.html. Talks to C++ through Browser.* / Boson.* RPC and the "tabs:updated" event.
inline constexpr std::string_view kShellHtml = R"BOSON(<!doctype html>
<html><head><meta charset="utf-8"><title>Boson Browser</title>
<style>
:root{color-scheme:dark;--bar:rgba(28,28,30,.92);--tab:rgba(255,255,255,.06);--tab-active:rgba(255,255,255,.16);--text:#f2f2f2;--muted:#a0a0a0;--accent:#4cc2ff}
*{box-sizing:border-box}html,body{margin:0;height:100%;background:transparent;font:12px "Segoe UI Variable","Segoe UI",sans-serif;color:var(--text);overflow:hidden;user-select:none}
#bar{position:fixed;left:0;top:0;right:0;height:40px;display:flex;align-items:center;gap:6px;padding-left:8px;background:var(--bar);app-region:drag;-webkit-app-region:drag}
#tabs{display:flex;gap:4px;min-width:0;flex:0 1 auto;overflow:hidden}
.tab{app-region:no-drag;-webkit-app-region:no-drag;display:flex;align-items:center;gap:6px;height:28px;max-width:200px;min-width:90px;padding:0 8px;border-radius:6px;background:var(--tab);cursor:default}
.tab.active{background:var(--tab-active)}.tab .t{overflow:hidden;white-space:nowrap;text-overflow:ellipsis;flex:1}
.tab .x{opacity:.6;border:0;background:none;color:inherit;width:18px;height:18px;border-radius:4px}.tab .x:hover{background:rgba(255,255,255,.15);opacity:1}
.tab .s{font-size:10px;color:var(--muted)}
button.i{app-region:no-drag;-webkit-app-region:no-drag;border:0;background:none;color:var(--text);height:28px;min-width:28px;border-radius:6px}button.i:hover{background:rgba(255,255,255,.1)}
#url{app-region:no-drag;-webkit-app-region:no-drag;flex:1 1 280px;min-width:120px;height:28px;border-radius:6px;border:1px solid rgba(255,255,255,.12);background:rgba(0,0,0,.35);color:var(--text);padding:0 10px;font:inherit}
#spacer{flex:0 0 12px;height:100%}
.win{app-region:no-drag;-webkit-app-region:no-drag;display:flex;height:100%}.win button{border:0;background:none;color:var(--text);width:46px;height:40px}
.win button:hover,.win button.hover{background:rgba(255,255,255,.1)}.win button.close:hover{background:#c42b1c}
#modal{position:fixed;inset:40px 0 0 0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.45)}
body.modal #modal{display:flex}
.card{width:420px;padding:20px;border-radius:12px;background:rgba(40,40,44,.82);border:1px solid rgba(255,255,255,.12);box-shadow:0 20px 60px rgba(0,0,0,.5)}
.card h2{margin:0 0 12px;font-size:16px}.card p{color:var(--muted);line-height:1.5}.row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.btn{border:0;border-radius:6px;padding:8px 12px;background:var(--accent);color:#000;font:inherit}.btn.ghost{background:rgba(255,255,255,.1);color:var(--text)}
pre{max-height:140px;overflow:auto;background:rgba(0,0,0,.35);padding:8px;border-radius:6px;font-size:11px}
</style></head><body>
<div id="bar">
  <div id="tabs"></div><button class="i" id="new" title="New tab">+</button>
  <button class="i" id="back" title="Back">&#8592;</button><button class="i" id="fwd" title="Forward">&#8594;</button><button class="i" id="reload" title="Reload">&#8635;</button>
  <input id="url" spellcheck="false" placeholder="Search or enter address">
  <button class="i" id="settings" title="Settings">&#9881;</button><div id="spacer"></div>
  <div class="win"><button id="min" title="Minimize">&#8212;</button><button id="max" title="Maximize">&#9744;</button><button id="close" class="close" title="Close">&#10005;</button></div>
</div>
<div id="modal"><div class="card">
  <h2>Settings</h2>
  <p id="mode"></p>
  <div class="row"><button class="btn" id="ss-live">Toggle screenshare mode (in place)</button><button class="btn ghost" id="ss-relaunch">Toggle via relaunch</button></div>
  <div class="row"><button class="btn ghost" id="procs">Show processes</button><button class="btn ghost" id="done">Close</button></div>
  <pre id="out"></pre>
</div></div>
<script>
const host = window.chrome.webview; let nextId = 1; const pending = new Map(); const listeners = {};
host.addEventListener('message', (e) => { const d = e.data; if (d && d.type === 'event') { (listeners[d.event] || []).forEach((f) => f(d.data)); return; }
  const p = pending.get(d.id); if (p) { pending.delete(d.id); d.status === 'success' ? p.res(d.data) : p.rej(d.error); } });
const call = (method, params) => new Promise((res, rej) => { const id = nextId++; pending.set(id, { res, rej }); host.postMessage({ id, method, params: params || {} }); });
const on = (event, f) => (listeners[event] = listeners[event] || []).push(f);
const $ = (id) => document.getElementById(id);
let state = { tabs: [], active: 0 };
function render(s) {
  state = s; const tabs = $('tabs'); tabs.textContent = '';
  for (const t of s.tabs) {
    const el = document.createElement('div'); el.className = 'tab' + (t.id === s.active ? ' active' : '');
    const badge = t.crashed ? '&#9888;' : t.loading ? '&#8987;' : t.audio ? (t.muted ? '&#128263;' : '&#128264;') : t.sleeping ? 'zz' : '';
    el.innerHTML = '<span class="s"></span><span class="t"></span><button class="x" title="Close tab">&#10005;</button>';
    el.querySelector('.s').innerHTML = badge; el.querySelector('.t').textContent = t.title || t.url || 'New tab'; el.title = t.url;
    el.addEventListener('mousedown', (e) => { if (e.button === 1) { call('Browser.close_tab', { id: t.id }); e.preventDefault(); } });
    el.addEventListener('click', (e) => { if (e.target.classList.contains('x')) call('Browser.close_tab', { id: t.id }); else call('Browser.activate_tab', { id: t.id }); });
    el.querySelector('.s').addEventListener('click', (e) => { if (t.audio) { e.stopPropagation(); call('Browser.toggle_mute', { id: t.id }); } });
    tabs.appendChild(el);
  }
  const a = s.tabs.find((t) => t.id === s.active);
  if (a && document.activeElement !== $('url')) $('url').value = a.url || '';
  $('back').disabled = !s.can_go_back; $('fwd').disabled = !s.can_go_forward;
  $('mode').textContent = 'Hosting: ' + s.hosting + '. Screenshare mode: ' + (s.screenshare_mode ? 'ON (DirectComposition off: PlayReady unavailable, Widevine video can be screen-shared)' : 'off (hardware DRM available)') + '.';
  document.body.classList.toggle('modal', s.modal);
}
on('tabs:updated', render);
on('window:maximize-button', (b) => $('max').classList.toggle('hover', b.hovered));
call('Browser.get_state').then(render);
$('new').onclick = () => call('Browser.new_tab', { url: '' });
$('back').onclick = () => call('Browser.back'); $('fwd').onclick = () => call('Browser.forward'); $('reload').onclick = () => call('Browser.reload');
$('url').addEventListener('keydown', (e) => { if (e.key === 'Enter') { call('Browser.navigate', { url: $('url').value }); $('url').blur(); } });
$('url').addEventListener('focus', () => $('url').select());
$('settings').onclick = () => call('Browser.set_modal', { enabled: true }); $('done').onclick = () => call('Browser.set_modal', { enabled: false });
$('ss-live').onclick = () => call('Browser.set_screenshare_mode', { enabled: !state.screenshare_mode });
$('ss-relaunch').onclick = () => call('Browser.relaunch', { enabled: !state.screenshare_mode });
$('procs').onclick = () => call('Browser.process_info').then((r) => { $('out').textContent = r.processes.map((p) => p.pid + '  ' + p.kind).join('\n'); });
$('min').onclick = () => call('Boson.minimize'); $('max').onclick = () => call('Boson.maximize_or_restore'); $('close').onclick = () => call('Boson.close');
const sendMax = () => { const r = $('max').getBoundingClientRect(); call('Boson.set_maximize_button_rect', { x: r.left, y: r.top, width: r.width, height: r.height, visible: true }); };
new ResizeObserver(sendMax).observe($('max')); window.addEventListener('resize', sendMax);
</script></body></html>)BOSON";

} // namespace boson::browser
