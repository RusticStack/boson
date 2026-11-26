// Shared reference-app logic, byte-identical in all four frameworks (Boson, Electron, Tauri v2, Wails v3).
// The framework supplies window.__cmp (adapter.js) with:
//   invoke(name, arg) -> Promise   one argument, JSON-able; names: config, ping, echo, items, burst, open_window, mark, report
//   listen(event, cb) -> Promise   "burst" events carry the event index as a bare number
// Timeline (the harness, bench/run_compare.ps1, only watches marks and process state; it never talks to the page):
//   paint (double rAF) -> ready (first IPC round trip) -> idle for idle_total_ms (no timers, no polling)
//   -> open the second window (it marks paint2/ready2) -> wait two_windows_ms -> run the IPC tests -> report.
(() => {
  'use strict';
  const second = new URLSearchParams(location.search).get('w') === '2';
  const wallMs = () => performance.timeOrigin + performance.now();
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const statusEl = document.getElementById('status');
  const setStatus = (text) => { statusEl.textContent = text; };
  const r3 = (v) => Math.round(v * 1000) / 1000;

  const paintedAt = new Promise((resolve) => {
    const start = () => requestAnimationFrame(() => requestAnimationFrame(() => resolve(wallMs())));
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, { once: true });
    else start();
  });

  const stats = (samples, total_ms) => {
    const s = [...samples].sort((a, b) => a - b);
    const q = (p) => {
      const r = (p / 100) * (s.length - 1);
      const lo = Math.floor(r);
      const hi = Math.min(lo + 1, s.length - 1);
      return s[lo] + (s[hi] - s[lo]) * (r - lo);
    };
    // mean_ms comes from the wall time of the whole loop, not from the (100 us quantised) per-call timer.
    return { n: s.length, mean_ms: r3(total_ms / s.length), p50_ms: r3(q(50)), p99_ms: r3(q(99)), max_ms: r3(s[s.length - 1]) };
  };

  // Deterministic pseudo-random printable payload (not trivially compressible, no JSON escapes).
  const makePayload = (bytes) => {
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let x = 0x2545f491;
    const chars = new Array(bytes);
    for (let i = 0; i < bytes; i++) {
      x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
      chars[i] = alphabet[x & 63];
    }
    return chars.join('');
  };

  async function sequential(count, call, check) {
    const lat = [];
    const t0 = performance.now();
    for (let i = 0; i < count; i++) {
      const t = performance.now();
      const v = await call(i);
      lat.push(performance.now() - t);
      if (!check(v, i)) throw new Error('unexpected reply at call ' + i);
    }
    return stats(lat, performance.now() - t0);
  }

  async function runIpc(cmp, cfg) {
    const out = { ok: false, error: '' };
    const step = (name) => cmp.invoke('mark', 'step_' + name + ' ' + wallMs()); // progress marks for diagnosing a stuck run
    try {
      for (let i = 0; i < 200; i++) await cmp.invoke('ping', i);
      out.ping_sequential = await sequential(cfg.ping_calls, (i) => cmp.invoke('ping', i), (v, i) => v === i);
      await step('ping');

      const conc = cfg.concurrent_calls;
      let t = performance.now();
      const replies = await Promise.all(Array.from({ length: conc }, (_, i) => cmp.invoke('ping', i)));
      let ms = performance.now() - t;
      if (replies.some((v, i) => v !== i)) throw new Error('concurrent ping mismatch');
      out.ping_concurrent = { n: conc, total_ms: r3(ms), calls_per_s: Math.round((conc / ms) * 1000) };
      await step('concurrent');

      out.echo = {};
      for (const [label, bytes, count] of [['1KiB', 1024, cfg.echo_1k_calls], ['64KiB', 64 * 1024, cfg.echo_64k_calls], ['1MiB', 1024 * 1024, cfg.echo_1m_calls]]) {
        const payload = makePayload(bytes);
        for (let i = 0; i < 3; i++) await cmp.invoke('echo', payload);
        const st = await sequential(count, () => cmp.invoke('echo', payload), (v) => typeof v === 'string' && v.length === bytes && v === payload);
        st.mb_per_s = r3((bytes / (1024 * 1024)) / (st.mean_ms / 1000)); // payload MiB per second, one direction
        out.echo[label] = st;
        await step('echo_' + label.toLowerCase());
      }

      for (let i = 0; i < 3; i++) await cmp.invoke('items');
      out.items_10k = await sequential(cfg.items_calls, () => cmp.invoke('items'),
        (v) => Array.isArray(v) && v.length === 10000 && v[9999].id === 9999 && typeof v[0].name === 'string');
      await step('items');

      let received = 0;
      let outOfOrder = 0;
      let target = 0;
      let done = null;
      await cmp.listen('burst', (n) => {
        if (n !== received) outOfOrder++;
        received++;
        if (done !== null && received >= target) { const d = done; done = null; d(); }
      });
      const burst = async (n) => {
        received = 0; outOfOrder = 0; target = n;
        const all = new Promise((resolve) => { done = resolve; });
        const timer = sleep(15000).then(() => 'timeout');
        const t0 = performance.now();
        await cmp.invoke('burst', n);
        const outcome = await Promise.race([all.then(() => 'ok'), timer]);
        const total = performance.now() - t0;
        done = null;
        return { n, received, out_of_order: outOfOrder, total_ms: r3(total), events_per_s: Math.round((received / total) * 1000), timeout: outcome === 'timeout' };
      };
      await burst(100);
      const runs = [];
      for (let i = 0; i < cfg.burst_runs; i++) runs.push(await burst(cfg.burst_events));
      const rates = runs.map((r) => r.events_per_s).sort((a, b) => a - b);
      out.events = { n: cfg.burst_events, runs, median_events_per_s: rates[Math.floor(rates.length / 2)] };
      out.ok = runs.every((r) => !r.timeout && r.received === cfg.burst_events);
      if (!out.ok) out.error = 'event burst lost events or timed out';
    } catch (err) {
      out.error = String((err && err.message) || err);
    }
    return out;
  }

  async function main() {
    const cmp = await window.__cmpReady.then(() => window.__cmp);
    const cfg = JSON.parse(await cmp.invoke('config'));
    if (!second) { // a second window may be occluded by the first, where Chromium pauses requestAnimationFrame
      const painted = await paintedAt;
      await cmp.invoke('mark', 'paint ' + painted);
    }
    const reply = await cmp.invoke('ping', 1);
    if (reply !== 1) throw new Error('first ping returned ' + reply);
    const ready = wallMs();
    await cmp.invoke('mark', (second ? 'ready2 ' : 'ready ') + ready);
    setStatus(second ? 'second window ready' : 'ready (idle)');
    if (second) return;

    await sleep(cfg.idle_total_ms);
    setStatus('opening second window');
    await cmp.invoke('mark', 'win2_requested ' + wallMs());
    await cmp.invoke('open_window');
    await sleep(cfg.two_windows_ms);
    setStatus('running IPC tests');
    await cmp.invoke('mark', 'ipc_start ' + wallMs());
    const result = await runIpc(cmp, cfg);
    setStatus('done');
    await cmp.invoke('report', JSON.stringify(result));
  }

  main().catch((err) => {
    setStatus('failed: ' + ((err && err.message) || err));
    if (!second && window.__cmp) window.__cmp.invoke('report', JSON.stringify({ ok: false, error: String((err && err.message) || err) })).catch(() => {});
  });
})();
