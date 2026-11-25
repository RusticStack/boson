import './style.css';
import { boson, BosonIpcError } from './boson';
import type {
  BackdropStatusResponse,
  BoolResponse,
  ComputePrimesResult,
  FileDialogResponse,
  NotifyRequest,
  OpenDialogRequest,
  OpacityRequest,
  PingResult,
  SystemInfo,
  ToggleRequest,
} from './generated/boson-bindings';

const app = document.querySelector<HTMLDivElement>('#app');
if (!app) {
  throw new Error('Root #app element not found');
}

const isWebView = boson.isAvailable();
if (!isWebView) {
  document.body.classList.add('solid-fallback');
}

app.innerHTML = `
  <nav class="titlebar">
    <div id="titlebar-drag" class="titlebar-drag" title="Drag to move, double-click to maximize">
      <span class="titlebar-dot"></span>
      <span class="titlebar-title">Boson &mdash; C++26 Fluent WebView2</span>
    </div>
    <div class="titlebar-controls">
      <button id="win-min" class="win-btn" title="Minimize" aria-label="Minimize">&#x2013;</button>
      <button id="win-max" class="win-btn" title="Maximize" aria-label="Maximize">&#x25A1;</button>
      <button id="win-close" class="win-btn close" title="Close" aria-label="Close">&#x2715;</button>
    </div>
  </nav>

  <main class="shell">
    <header class="header">
      <div class="brand">
        <div class="brand-badge">B</div>
        <div>
          <h1>Boson C++26 WebView2</h1>
          <p>Fluent Mica/Acrylic &bull; Headless Titlebar &bull; Native Notifications &bull; stdexec IPC</p>
        </div>
      </div>

      <div class="fps-widget" title="Runs on main UI thread via requestAnimationFrame">
        <div class="ring-container">
          <svg class="ring-svg" viewBox="0 0 44 44">
            <circle class="ring-track" cx="22" cy="22" r="18" />
            <circle id="raf-arc" class="ring-arc" cx="22" cy="22" r="18" />
          </svg>
        </div>
        <div class="fps-meta">
          <span id="fps-value" class="fps-value">60 FPS</span>
          <span class="fps-label">UI Thread Lock-Free</span>
        </div>
      </div>
    </header>

    <section class="grid">
      <article class="card full-width">
        <div class="card-head">
          <div>
            <div class="card-title">Windows Fluent Design &amp; Native OS</div>
            <div class="card-sub">DWM backdrops, window translucency, native notifications, and COM file dialogs</div>
          </div>
          <span class="tag">Boson.*</span>
        </div>

        <div class="fluent-controls">
          <div class="pill-group" id="backdrop-pills">
            <button class="pill active" data-backdrop="mica">Mica</button>
            <button class="pill" data-backdrop="mica_alt">Mica Alt</button>
            <button class="pill" data-backdrop="acrylic">Acrylic</button>
            <button class="pill" data-backdrop="transparent">Transparent</button>
            <button class="pill" data-backdrop="translucent">Translucent</button>
            <button class="pill" data-backdrop="none">Opaque</button>
          </div>

          <label class="slider-wrap">
            <span>Opacity</span>
            <input id="opacity-slider" type="range" min="30" max="100" value="100" />
            <span id="opacity-label">100%</span>
          </label>
        </div>

        <div class="fluent-controls">
          <button id="btn-notify" class="btn">Send Native Notification</button>
          <button id="btn-open-file" class="btn">Open File Dialog</button>
          <button id="btn-open-folder" class="btn">Pick Folder</button>
          <button id="btn-topmost" class="pill">Always on Top: Off</button>
          <button id="btn-theme" class="pill active">Dark Mode: On</button>
        </div>

        <pre id="out-native" class="output">Active backdrop: Mica (transparent WebView2 surface).</pre>
      </article>

      <article class="card">
        <div class="card-head">
          <div>
            <div class="card-title">System Information</div>
            <div class="card-sub">Query Win32 &amp; WebView2 runtime metadata</div>
          </div>
          <span class="tag">App.get_system_info</span>
        </div>
        <button id="btn-sysinfo" class="btn">Fetch System Info</button>
        <pre id="out-sysinfo" class="output">Ready.</pre>
      </article>

      <article class="card">
        <div class="card-head">
          <div>
            <div class="card-title">Heavy C++ Compute (stdexec)</div>
            <div class="card-sub">Prime sieve up to 2,000,000 off the UI thread</div>
          </div>
          <span class="tag">App.compute_primes</span>
        </div>
        <button id="btn-primes" class="btn">Compute Primes (2M)</button>
        <pre id="out-primes" class="output">Watch the 60 FPS ring stay smooth during execution.</pre>
      </article>

      <article class="card">
        <div class="card-head">
          <div>
            <div class="card-title">ABI Exception Boundary</div>
            <div class="card-sub">C++ exception caught &amp; rejected as JS Promise</div>
          </div>
          <span class="tag danger">App.trigger_error</span>
        </div>
        <button id="btn-error" class="btn danger">Trigger C++ Exception</button>
        <pre id="out-error" class="output">Ready.</pre>
      </article>

      <article class="card">
        <div class="card-head">
          <div>
            <div class="card-title">IPC Latency Benchmark</div>
            <div class="card-sub">1,000 round-trip calls (sequential &amp; parallel)</div>
          </div>
          <span class="tag">App.ping &times; 1000</span>
        </div>
        <button id="btn-bench" class="btn">Run 1000x Ping Benchmark</button>
        <pre id="out-bench" class="output">Ready.</pre>
      </article>
    </section>

    <footer class="status-bar">
      <span>
        <span class="dot ${isWebView ? '' : 'offline'}"></span>
        ${isWebView ? 'Connected to native Boson WebView2 bridge' : 'Browser preview mode (WebView2 bridge not attached)'}
      </span>
      <span>Scheme: ${window.location.origin}</span>
    </footer>
  </main>
`;

// Headless Titlebar Controls
document.querySelector('#titlebar-drag')?.addEventListener('mousedown', (e) => {
  const ev = e as MouseEvent;
  if (ev.button === 0 && ev.detail === 1) {
    void boson.call('Boson.start_drag').catch(() => {});
  }
});
document.querySelector('#titlebar-drag')?.addEventListener('dblclick', () => {
  void boson.call<BoolResponse>('Boson.maximize_or_restore').catch(() => {});
});
document.querySelector('#win-min')?.addEventListener('click', () => {
  void boson.call('Boson.minimize').catch(() => {});
});
document.querySelector('#win-max')?.addEventListener('click', () => {
  void boson.call<BoolResponse>('Boson.maximize_or_restore').catch(() => {});
});
document.querySelector('#win-close')?.addEventListener('click', () => {
  void boson.call('Boson.close').catch(() => {});
});

// Live 60fps RAF animation ring + FPS counter
const rafArc = document.querySelector<SVGCircleElement>('#raf-arc')!;
const fpsValue = document.querySelector<HTMLSpanElement>('#fps-value')!;
let angle = 0;
let frames = 0;
let lastFpsSample = performance.now();
let lastFrameTime = performance.now();

function animateRing(now: number): void {
  const dt = Math.min(now - lastFrameTime, 100);
  lastFrameTime = now;
  angle = (angle + dt * 0.24) % 360;
  rafArc.style.transform = `rotate(${angle.toFixed(1)}deg)`;
  rafArc.style.transformOrigin = '22px 22px';

  frames++;
  const elapsed = now - lastFpsSample;
  if (elapsed >= 500) {
    const fps = Math.round((frames * 1000) / elapsed);
    fpsValue.textContent = `${fps} FPS`;
    fpsValue.style.color = fps >= 50 ? 'var(--success)' : fps >= 30 ? 'var(--warning)' : 'var(--danger)';
    frames = 0;
    lastFpsSample = now;
  }

  requestAnimationFrame(animateRing);
}
requestAnimationFrame(animateRing);

function setOutput(el: HTMLPreElement, text: string, state: 'ok' | 'err' | 'neutral' = 'neutral'): void {
  el.textContent = text;
  el.classList.remove('ok', 'err');
  if (state !== 'neutral') {
    el.classList.add(state);
  }
}

// Fluent & Native OS Controls
const outNative = document.querySelector<HTMLPreElement>('#out-native')!;
const backdropPills = document.querySelectorAll<HTMLButtonElement>('#backdrop-pills .pill');
backdropPills.forEach((pill) => {
  pill.addEventListener('click', async () => {
    const backdrop = pill.dataset.backdrop ?? 'mica';
    backdropPills.forEach((p) => p.classList.remove('active'));
    pill.classList.add('active');
    document.body.classList.toggle('solid-fallback', backdrop === 'none' || !isWebView);
    try {
      const res = await boson.call<BackdropStatusResponse, { backdrop: string }>('Boson.set_backdrop', {
        backdrop,
      });
      setOutput(
        outNative,
        `Backdrop set to "${res.backdrop}" (transparent WebView2: ${res.transparent_webview})`,
        'ok'
      );
    } catch (err) {
      setOutput(outNative, formatError(err), 'err');
    }
  });
});

const opacitySlider = document.querySelector<HTMLInputElement>('#opacity-slider')!;
const opacityLabel = document.querySelector<HTMLSpanElement>('#opacity-label')!;
opacitySlider.addEventListener('input', () => {
  const pct = Number(opacitySlider.value);
  opacityLabel.textContent = `${pct}%`;
  void boson.call<void, OpacityRequest>('Boson.set_opacity', { opacity: pct / 100 }).catch(() => {});
});

let isTopmost = false;
const btnTopmost = document.querySelector<HTMLButtonElement>('#btn-topmost')!;
btnTopmost.addEventListener('click', async () => {
  isTopmost = !isTopmost;
  btnTopmost.classList.toggle('active', isTopmost);
  btnTopmost.textContent = `Always on Top: ${isTopmost ? 'On' : 'Off'}`;
  try {
    await boson.call<void, ToggleRequest>('Boson.set_always_on_top', { enabled: isTopmost });
  } catch (err) {
    setOutput(outNative, formatError(err), 'err');
  }
});

let isDarkMode = true;
const btnTheme = document.querySelector<HTMLButtonElement>('#btn-theme')!;
btnTheme.addEventListener('click', async () => {
  isDarkMode = !isDarkMode;
  btnTheme.classList.toggle('active', isDarkMode);
  btnTheme.textContent = `Dark Mode: ${isDarkMode ? 'On' : 'Off'}`;
  try {
    await boson.call<void, ToggleRequest>('Boson.set_dark_mode', { enabled: isDarkMode });
  } catch (err) {
    setOutput(outNative, formatError(err), 'err');
  }
});

document.querySelector('#btn-notify')?.addEventListener('click', async () => {
  try {
    const res = await boson.call<BoolResponse, NotifyRequest>('Boson.notify', {
      title: 'Boson Desktop',
      body: 'Native Windows notification dispatched from C++26!',
      level: 'info',
      silent: false,
    });
    setOutput(outNative, `Notification sent (delivered=${res.value})`, res.value ? 'ok' : 'err');
  } catch (err) {
    setOutput(outNative, formatError(err), 'err');
  }
});

document.querySelector('#btn-open-file')?.addEventListener('click', async () => {
  try {
    const res = await boson.call<FileDialogResponse, OpenDialogRequest>('Boson.open_file_dialog', {
      title: 'Select Files',
      allow_multiple: true,
      pick_folder: false,
    });
    setOutput(
      outNative,
      res.cancelled ? 'File dialog cancelled.' : `Selected:\n${res.paths.join('\n')}`,
      'ok'
    );
  } catch (err) {
    setOutput(outNative, formatError(err), 'err');
  }
});

document.querySelector('#btn-open-folder')?.addEventListener('click', async () => {
  try {
    const res = await boson.call<FileDialogResponse, OpenDialogRequest>('Boson.open_file_dialog', {
      title: 'Select Folder',
      allow_multiple: false,
      pick_folder: true,
    });
    setOutput(
      outNative,
      res.cancelled ? 'Folder dialog cancelled.' : `Selected folder:\n${res.paths.join('\n')}`,
      'ok'
    );
  } catch (err) {
    setOutput(outNative, formatError(err), 'err');
  }
});

// AppController Demos
const btnSysInfo = document.querySelector<HTMLButtonElement>('#btn-sysinfo')!;
const outSysInfo = document.querySelector<HTMLPreElement>('#out-sysinfo')!;
btnSysInfo.addEventListener('click', async () => {
  btnSysInfo.disabled = true;
  const t0 = performance.now();
  try {
    const info = await boson.call<SystemInfo>('App.get_system_info');
    const ms = (performance.now() - t0).toFixed(2);
    setOutput(
      outSysInfo,
      `OS:          ${info.os}\nArch:        ${info.architecture}\nCPU Cores:   ${info.cpu_cores}\nWebView2:    ${info.webview2_runtime}\nBuild Mode:  ${info.build_mode}\nRound-trip:  ${ms} ms`,
      'ok'
    );
  } catch (err) {
    setOutput(outSysInfo, formatError(err), 'err');
  } finally {
    btnSysInfo.disabled = false;
  }
});

const btnPrimes = document.querySelector<HTMLButtonElement>('#btn-primes')!;
const outPrimes = document.querySelector<HTMLPreElement>('#out-primes')!;
btnPrimes.addEventListener('click', async () => {
  btnPrimes.disabled = true;
  setOutput(outPrimes, 'Running sieve on stdexec worker pool...');
  const t0 = performance.now();
  try {
    const res = await boson.call<ComputePrimesResult, { limit: number }>('App.compute_primes', {
      limit: 2_000_000,
    });
    const wallMs = (performance.now() - t0).toFixed(2);
    setOutput(
      outPrimes,
      `Limit:         ${res.limit.toLocaleString()}\nPrimes Found:  ${res.count.toLocaleString()}\nLargest Prime: ${res.largest_prime.toLocaleString()}\nC++ Compute:   ${res.elapsed_ms.toFixed(2)} ms\nTotal IPC:     ${wallMs} ms`,
      'ok'
    );
  } catch (err) {
    setOutput(outPrimes, formatError(err), 'err');
  } finally {
    btnPrimes.disabled = false;
  }
});

const btnError = document.querySelector<HTMLButtonElement>('#btn-error')!;
const outError = document.querySelector<HTMLPreElement>('#out-error')!;
btnError.addEventListener('click', async () => {
  btnError.disabled = true;
  try {
    await boson.call('App.trigger_error');
    setOutput(outError, 'Unexpected success', 'err');
  } catch (err) {
    setOutput(outError, `Rejected cleanly:\n${formatError(err)}`, 'ok');
  } finally {
    btnError.disabled = false;
  }
});

const btnBench = document.querySelector<HTMLButtonElement>('#btn-bench')!;
const outBench = document.querySelector<HTMLPreElement>('#out-bench')!;
btnBench.addEventListener('click', async () => {
  btnBench.disabled = true;
  setOutput(outBench, 'Running 1,000 sequential + 1,000 parallel pings...');
  try {
    const seqLatencies: number[] = [];
    const seqStart = performance.now();
    for (let i = 0; i < 1000; i++) {
      const t0 = performance.now();
      await boson.call<PingResult>('App.ping');
      seqLatencies.push(performance.now() - t0);
    }
    const seqTotal = performance.now() - seqStart;
    seqLatencies.sort((a, b) => a - b);
    const seqAvg = seqLatencies.reduce((a, b) => a + b, 0) / seqLatencies.length;
    const seqP99 = seqLatencies[Math.floor(seqLatencies.length * 0.99)];

    const parStart = performance.now();
    await Promise.all(Array.from({ length: 1000 }, () => boson.call<PingResult>('App.ping')));
    const parTotal = performance.now() - parStart;

    setOutput(
      outBench,
      `Sequential (1000x):\n  Total: ${seqTotal.toFixed(1)} ms | Avg: ${seqAvg.toFixed(3)} ms | P99: ${seqP99.toFixed(3)} ms\nParallel (1000x burst):\n  Total: ${parTotal.toFixed(1)} ms (${Math.round(1000 / (parTotal / 1000)).toLocaleString()} req/s)`,
      'ok'
    );
  } catch (err) {
    setOutput(outBench, formatError(err), 'err');
  } finally {
    btnBench.disabled = false;
  }
});

function formatError(err: unknown): string {
  if (err instanceof BosonIpcError) {
    return `[BosonIpcError code=${err.code}] ${err.message}`;
  }
  if (err instanceof Error) {
    return `${err.name}: ${err.message}`;
  }
  return String(err);
}
