// Browser benchmark, shell side. Shared verbatim by examples/browser (Boson) and bench/wails_browser (Wails v3)
// so both frameworks run exactly the same measurement code. Each app installs window.__benchHost first:
//   painted()            first frame of the shell is on screen (double requestAnimationFrame)
//   ping(seq) -> seq     smallest round trip
//   echo(text) -> text   payload round trip
//   emitBurst(n)         host emits n "bench:tick" events as fast as it can, then resolves
//   onTick(cb)           subscribe to "bench:tick"
//   onRunIpc(cb)         host asks the shell to run the IPC tests
//   report(json)         hand the IPC results (a JSON string) back to the host
(() => {
  'use strict';
  const host = window.__benchHost;
  if (!host || window.__benchShellInstalled) return;
  window.__benchShellInstalled = true;

  const r3 = (v) => Math.round(v * 1000) / 1000;
  const sorted = (xs) => [...xs].sort((a, b) => a - b);
  const quantile = (s, p) => {
    if (s.length === 0) return 0;
    const r = (p / 100) * (s.length - 1);
    const lo = Math.floor(r);
    const hi = Math.min(lo + 1, s.length - 1);
    return s[lo] + (s[hi] - s[lo]) * (r - lo);
  };
  const stats = (xs) => {
    const s = sorted(xs);
    const mean = s.reduce((a, b) => a + b, 0) / Math.max(1, s.length);
    return { n: s.length, mean_ms: r3(mean), p50_ms: r3(quantile(s, 50)), p95_ms: r3(quantile(s, 95)),
      p99_ms: r3(quantile(s, 99)), min_ms: r3(s[0] || 0), max_ms: r3(s[s.length - 1] || 0) };
  };

  const firstPaint = () => requestAnimationFrame(() => requestAnimationFrame(() => { void host.painted(); }));
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', firstPaint, { once: true });
  else firstPaint();

  let ticks = 0;
  let tickTarget = 0;
  let tickDone = null;
  host.onTick(() => {
    ticks++;
    if (tickDone !== null && ticks >= tickTarget) {
      const done = tickDone;
      tickDone = null;
      done('ok');
    }
  });

  async function emitBurst(n) {
    ticks = 0;
    tickTarget = n;
    const all = new Promise((resolve) => { tickDone = resolve; });
    const timeout = new Promise((resolve) => setTimeout(() => resolve('timeout'), 30000));
    const t0 = performance.now();
    await host.emitBurst(n);
    const outcome = await Promise.race([all, timeout]);
    const ms = performance.now() - t0;
    tickDone = null;
    return { n, received: ticks, total_ms: r3(ms), events_per_s: Math.round((ticks / ms) * 1000), timeout: outcome === 'timeout' };
  }

  const CALLS = 1000;
  const PAYLOAD = 1024 * 1024;
  const REPS = 5;

  async function run() {
    const out = { ok: false, error: '' };
    try {
      for (let i = 0; i < 50; i++) await host.ping(i);
      const lat = [];
      for (let i = 0; i < CALLS; i++) {
        const t = performance.now();
        const v = await host.ping(i);
        lat.push(performance.now() - t);
        if (v !== i) throw new Error('ping returned ' + v + ' for ' + i);
      }
      out.ping_sequential = stats(lat);

      let t = performance.now();
      await Promise.all(Array.from({ length: CALLS }, (_, i) => host.ping(i)));
      let ms = performance.now() - t;
      out.ping_concurrent = { n: CALLS, total_ms: r3(ms), calls_per_s: Math.round((CALLS / ms) * 1000) };

      const big = 'x'.repeat(PAYLOAD);
      const echo = [];
      for (let i = 0; i < REPS; i++) {
        t = performance.now();
        const r = await host.echo(big);
        echo.push(performance.now() - t);
        if (typeof r !== 'string' || r.length !== PAYLOAD) throw new Error('echo size mismatch');
      }
      const med = quantile(sorted(echo), 50);
      out.echo_1mib = { runs_ms: echo.map(r3), median_ms: r3(med), mb_per_s: Math.round((1 / (med / 1000)) * 10) / 10 };

      await emitBurst(100); // warm-up
      out.emit_burst = await emitBurst(10000);
      out.ok = true;
    } catch (err) {
      out.error = String((err && err.message) || err);
    }
    await host.report(JSON.stringify(out));
  }

  host.onRunIpc(() => { void run(); });
})();
