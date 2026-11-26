#pragma once

#include <string_view>

namespace addon_mini {

/// Served from memory as boson://app/index.html (no bundler needed for the example).
inline constexpr std::string_view kPageHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Addon Manager Mini</title>
<style>
html,body{margin:0;background:#18181c;color:#e8e8ea;font:13px/1.5 "Segoe UI",system-ui,sans-serif}
main{padding:18px 22px;max-width:900px}h1{font-size:17px;margin:0 0 4px}h2{font-size:14px;margin:18px 0 6px;color:#aaa}
table{width:100%;border-collapse:collapse}td,th{padding:6px 8px;border-bottom:1px solid #2a2a30;text-align:left}
button{background:#3a5bd9;color:#fff;border:0;border-radius:4px;padding:4px 12px;cursor:pointer}button.secondary{background:#444}
button:disabled{opacity:.5;cursor:default}progress{width:160px}.err{color:#ff7b7b}.ok{color:#7bd88f}small{color:#888}
</style></head>
<body><main>
<h1>Addon Manager Mini</h1><small id="dir"></small>
<h2>Available</h2><table id="feed"><tr><th>Addon</th><th>Version</th><th>Size</th><th></th><th>Status</th></tr></table>
<h2>Installed</h2><table id="installed"><tr><th>Addon</th><th>Version</th><th>Folders</th><th></th></tr></table>
</main>
<script>
(() => {
  const host = window.chrome && window.chrome.webview;
  if (!host) { document.body.textContent = 'window.chrome.webview is unavailable'; return; }
  const pending = new Map(); let nextId = 1; const listeners = {};
  host.addEventListener('message', (e) => {
    const d = e.data;
    if (d && d.type === 'event') { (listeners[d.event] || []).forEach((f) => f(d.data)); return; }
    const p = d && pending.get(d.id); if (!p) return; pending.delete(d.id);
    if (d.status === 'success') p.resolve(d.data); else p.reject(new Error(d.error ? d.error.message : 'IPC error'));
  });
  const call = (method, params) => new Promise((resolve, reject) => {
    const id = nextId++; pending.set(id, { resolve, reject }); host.postMessage({ id, method, params: params === undefined ? {} : params });
  });
  const on = (ev, f) => { (listeners[ev] = listeners[ev] || []).push(f); };
  const el = (tag, text) => { const n = document.createElement(tag); if (text !== undefined) n.textContent = text; return n; };
  const kb = (n) => (n / 1024).toFixed(1) + ' KiB';
  const status = {};
  const clearRows = (t) => { while (t.rows.length > 1) t.deleteRow(1); };

  async function refresh() {
    const [feed, inst] = await Promise.all([call('Addons.feed'), call('Addons.installed')]);
    document.getElementById('dir').textContent = inst.addons_dir;
    const installed = new Map(inst.addons.map((a) => [a.uid, a]));
    const ft = document.getElementById('feed'); clearRows(ft);
    for (const a of feed.addons) {
      const tr = ft.insertRow(); tr.append(el('td', a.name), el('td', a.version), el('td', kb(a.size)));
      const td = el('td'); const b = el('button', installed.has(a.uid) ? 'Reinstall' : 'Install');
      b.onclick = () => { b.disabled = true; status[a.uid].textContent = 'starting...'; void call('Addons.install', { uid: a.uid }); };
      td.append(b); tr.append(td);
      const st = el('td'); status[a.uid] = st; tr.append(st);
    }
    const it = document.getElementById('installed'); clearRows(it);
    for (const a of inst.addons) {
      const tr = it.insertRow(); tr.append(el('td', a.name), el('td', a.version), el('td', a.folders.join(', ')));
      const td = el('td'); const b = el('button', 'Uninstall'); b.className = 'secondary';
      b.onclick = async () => { b.disabled = true; await call('Addons.uninstall', { uid: a.uid }); await refresh(); };
      td.append(b); tr.append(td);
    }
  }
  on('addons:progress', (p) => {
    const st = status[p.uid]; if (!st) return;
    st.textContent = '';
    if (p.phase === 'download' && p.total) { const bar = el('progress'); bar.max = p.total; bar.value = p.received; st.append(bar, el('small', ' ' + kb(p.received))); }
    else st.textContent = p.phase + '...';
  });
  on('addons:done', async (d) => {
    await refresh();
    const st = status[d.uid]; if (!st) return;
    st.className = d.ok ? 'ok' : 'err'; st.textContent = d.ok ? 'installed' : d.error;
  });
  refresh().catch((e) => { document.body.append(el('pre', String(e))); });
})();
</script></body></html>)HTML";

} // namespace addon_mini
