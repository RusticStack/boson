#pragma once

#include <string_view>

namespace boson::bench_app {

/// Served from memory as boson://app/index.html. A tiny hand-written RPC client (no Vite build needed)
/// so the measurement does not depend on the demo frontend bundle.
inline constexpr std::string_view kPageHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Boson Bench App</title>
<style>
html,body{margin:0;background:#1b1b1f;color:#e6e6e6;font:13px/1.5 "Segoe UI",system-ui,sans-serif}
main{padding:16px 20px}h1{font-size:16px;margin:0 0 8px}pre{white-space:pre-wrap;background:#111;padding:10px;border-radius:6px}
</style></head>
<body><main><h1>Boson IPC benchmark</h1><div id="status">idle - waiting for the harness (BosonBench.StartLoad)</div><pre id="out"></pre></main>
<script>
(() => {
  const host = window.chrome && window.chrome.webview;
  const statusEl = document.getElementById('status');
  const outEl = document.getElementById('out');
  if (!host) { statusEl.textContent = 'window.chrome.webview is unavailable'; return; }
  const pending = new Map();
  let nextId = 1;
  let running = false;
  let tickCount = 0, tickTarget = 0, tickDone = null;
  const handle = (d) => {
    if (d && d.type === 'event') {
      if (d.event === 'bench:tick') { if (++tickCount === tickTarget && tickDone) tickDone(); return; }
      if (d.event === 'bench:start') { void runAll(); }
      return;
    }
    const p = d && pending.get(d.id);
    if (!p) return;
    pending.delete(d.id);
    if (d.status === 'success') p.resolve(d.data); else p.reject(new Error(d.error ? d.error.message : 'IPC error'));
  };
  host.addEventListener('message', (e) => {
    const d = e.data;
    if (Array.isArray(d)) { for (const item of d) handle(item); } else { handle(d); }
  });
  const call = (method, params) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    host.postMessage({ id, method, params: params === undefined ? {} : params });
  });
  const stats = (xs) => {
    const s = [...xs].sort((a, b) => a - b);
    const q = (p) => { const r = p / 100 * (s.length - 1); const lo = Math.floor(r); const hi = Math.min(lo + 1, s.length - 1); return s[lo] + (s[hi] - s[lo]) * (r - lo); };
    const mean = s.reduce((a, b) => a + b, 0) / s.length;
    const r3 = (v) => Math.round(v * 1000) / 1000;
    return { n: s.length, mean_ms: r3(mean), p50_ms: r3(q(50)), p95_ms: r3(q(95)), p99_ms: r3(q(99)), min_ms: r3(s[0]), max_ms: r3(s[s.length - 1]) };
  };
  const median = (xs) => { const s = [...xs].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };
  const SMALL = 1000;
  const BIG = 10 * 1024 * 1024;
  const REPS = 3;
  async function sequential(fn) {
    const lat = [];
    for (let i = 0; i < SMALL; i++) { const t = performance.now(); await fn(i); lat.push(performance.now() - t); }
    return stats(lat);
  }
  async function runAll() {
    if (running) return;
    running = true;
    statusEl.textContent = 'running...';
    const result = { small_calls: SMALL, payload_bytes: BIG };
    try {
      for (let i = 0; i < 50; i++) { await call('Bench.ping', { seq: i }); await call('ElectronIpc.invoke', { channel: 'bench:ping', args: [i] }); }
      result.static_sequential = await sequential((i) => call('Bench.ping', { seq: i }));
      result.dynamic_sequential = await sequential((i) => call('ElectronIpc.invoke', { channel: 'bench:ping', args: [i] }));
      let t = performance.now();
      await Promise.all(Array.from({ length: SMALL }, (_, i) => call('Bench.ping', { seq: i })));
      let ms = performance.now() - t;
      result.static_concurrent = { n: SMALL, total_ms: Math.round(ms * 1000) / 1000, calls_per_s: Math.round(SMALL / ms * 1000) };
      t = performance.now();
      await Promise.all(Array.from({ length: SMALL }, (_, i) => call('ElectronIpc.invoke', { channel: 'bench:ping', args: [i] })));
      ms = performance.now() - t;
      result.dynamic_concurrent = { n: SMALL, total_ms: Math.round(ms * 1000) / 1000, calls_per_s: Math.round(SMALL / ms * 1000) };

      const bursts = [];
      for (let i = 0; i < REPS; i++) {
        const n = 20000;
        tickCount = 0; tickTarget = n;
        const done = new Promise((r) => { tickDone = r; });
        t = performance.now();
        await call('Bench.emit_burst', { n });
        await done;
        bursts.push(n / (performance.now() - t) * 1000);
      }
      result.events_20k = { runs_per_s: bursts.map(Math.round), median_per_s: Math.round(median(bursts)) };

      const big = 'x'.repeat(BIG);
      const up = [];
      for (let i = 0; i < REPS; i++) {
        t = performance.now();
        const r = await call('Bench.upload', { data: big });
        up.push(performance.now() - t);
        if (r.bytes !== BIG) throw new Error('upload size mismatch: ' + r.bytes);
      }
      const down = [];
      for (let i = 0; i < REPS; i++) {
        t = performance.now();
        const r = await call('Bench.download', { bytes: BIG });
        down.push(performance.now() - t);
        if (!r || r.data.length !== BIG) throw new Error('download size mismatch');
      }
      const echo = [];
      for (let i = 0; i < REPS; i++) {
        t = performance.now();
        const r = await call('Bench.echo', { data: big });
        echo.push(performance.now() - t);
        if (r.data.length !== BIG) throw new Error('echo size mismatch');
      }
      const mbps = (v) => Math.round(BIG / (1024 * 1024) / (v / 1000) * 10) / 10;
      const r1 = (v) => Math.round(v * 10) / 10;
      result.upload_10mb = { runs_ms: up.map(r1), median_ms: r1(median(up)), mb_per_s: mbps(median(up)) };
      result.download_10mb = { runs_ms: down.map(r1), median_ms: r1(median(down)), mb_per_s: mbps(median(down)) };
      result.echo_10mb = { runs_ms: echo.map(r1), median_ms: r1(median(echo)), mb_per_s: mbps(median(echo)) };
      result.ok = true;
    } catch (err) {
      result.ok = false;
      result.error = String(err && err.message || err);
    }
    outEl.textContent = JSON.stringify(result, null, 2);
    statusEl.textContent = result.ok ? 'done' : 'failed';
    await call('Bench.report', result);
    running = false;
  }
  requestAnimationFrame(() => requestAnimationFrame(() => { void call('Bench.ready'); }));
})();
</script></body></html>
)HTML";

} // namespace boson::bench_app
