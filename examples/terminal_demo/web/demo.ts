// Split-pane terminal demo: every pane is a native ConPTY (boson::terminal) rendered by xterm.js + WebGL.
// Output streams over BinaryTransport shared buffers; keystrokes take the fast input path; hidden panes stop
// receiving output (the native ring keeps it) and replay when shown.
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import { WebglAddon } from '@xterm/addon-webgl';
import { attachXterm, spawnTerminal, type BosonTerminal } from '../../../frontend/src/terminal';

interface Pane {
  el: HTMLElement;
  xterm: Terminal;
  term: BosonTerminal;
  fit: FitAddon;
  detach: () => void;
  renderer: 'webgl' | 'dom';
  hidden: boolean;
  bytes: number;
}

const root = document.getElementById('root') as HTMLElement;
const status = document.getElementById('status') as HTMLElement;
const panes: Pane[] = [];
let focused: Pane | undefined;

function setFocus(p: Pane | undefined): void {
  for (const q of panes) q.el.classList.toggle('focused', q === p);
  focused = p;
  if (p && !p.hidden) p.xterm.focus();
}

function bar(p: Pane): void {
  const title = p.el.querySelector('.title') as HTMLElement;
  const meta = p.el.querySelector('.meta') as HTMLElement;
  title.textContent = p.term.title || 'terminal ' + p.term.id;
  const exit = p.term.exitCode !== undefined ? ' exited ' + p.term.exitCode : '';
  meta.textContent = 'pid ' + p.term.pid + ' ' + p.renderer + (p.hidden ? ' (hidden)' : '') + exit;
}

async function createPane(container: HTMLElement, commandLine?: string): Promise<Pane> {
  const el = document.createElement('div');
  el.className = 'pane';
  el.innerHTML = '<div class="bar"><span class="title"></span><span class="meta"></span></div><div class="term"></div>';
  container.appendChild(el);
  const host = el.querySelector('.term') as HTMLElement;
  const xterm = new Terminal({
    fontFamily: 'Cascadia Mono, Consolas, monospace',
    fontSize: 13,
    scrollback: 10000,
    allowProposedApi: true,
    theme: { background: '#1e1e1e' },
  });
  const fit = new FitAddon();
  xterm.loadAddon(fit);
  xterm.open(host);
  let renderer: Pane['renderer'] = 'dom';
  try {
    const webgl = new WebglAddon();
    webgl.onContextLoss(() => webgl.dispose());
    xterm.loadAddon(webgl);
    renderer = 'webgl';
  } catch {
    renderer = 'dom'; // no WebGL2 (e.g. GPU blocklisted): xterm falls back to its DOM renderer
  }
  fit.fit();
  const term = await spawnTerminal({ commandLine, cols: xterm.cols, rows: xterm.rows });
  const p: Pane = { el, xterm, term, fit, detach: () => {}, renderer, hidden: false, bytes: 0 };
  p.detach = attachXterm(term, xterm, { visibilityElement: host });
  term.onData((d) => {
    p.bytes += d.byteLength;
  });
  term.onTitle(() => bar(p));
  term.onExit(() => bar(p));
  el.addEventListener('mousedown', () => setFocus(p));
  new ResizeObserver(() => {
    if (!p.hidden) p.fit.fit();
  }).observe(host);
  panes.push(p);
  bar(p);
  setFocus(p);
  return p;
}

async function split(direction: 'h' | 'v', commandLine?: string): Promise<Pane> {
  const target = focused;
  if (!target) {
    const s = document.createElement('div');
    s.className = 'split ' + direction;
    root.appendChild(s);
    return createPane(s, commandLine);
  }
  const parent = target.el.parentElement as HTMLElement;
  const s = document.createElement('div');
  s.className = 'split ' + direction;
  parent.replaceChild(s, target.el);
  s.appendChild(target.el);
  const p = await createPane(s, commandLine);
  for (const q of panes) if (!q.hidden) q.fit.fit();
  return p;
}

async function closePane(p: Pane): Promise<void> {
  p.detach();
  await p.term.dispose();
  p.xterm.dispose();
  const parent = p.el.parentElement as HTMLElement;
  p.el.remove();
  panes.splice(panes.indexOf(p), 1);
  // Collapse a split left with one child.
  if (parent.classList.contains('split') && parent.children.length === 1 && parent !== root) {
    const only = parent.firstElementChild as HTMLElement;
    const grand = parent.parentElement as HTMLElement;
    if (grand !== root || root.children.length > 1) grand.replaceChild(only, parent);
  }
  if (parent.children.length === 0) parent.remove();
  setFocus(panes[panes.length - 1]);
  for (const q of panes) if (!q.hidden) q.fit.fit();
}

function setHidden(p: Pane, hidden: boolean): void {
  p.hidden = hidden;
  p.el.classList.toggle('hidden-pane', hidden);
  void p.term.setVisible(!hidden); // IntersectionObserver also notices; explicit is immediate
  if (!hidden) p.fit.fit();
  bar(p);
}

document.getElementById('split-h')?.addEventListener('click', () => void split('h'));
document.getElementById('split-v')?.addEventListener('click', () => void split('v'));
document.getElementById('new-ps')?.addEventListener('click', () => void split('h', 'powershell.exe -NoLogo'));
document.getElementById('close')?.addEventListener('click', () => focused && void closePane(focused));
document.getElementById('toggle')?.addEventListener('click', () => focused && setHidden(focused, !focused.hidden));
window.addEventListener('keydown', (e) => {
  if (!e.ctrlKey || !e.shiftKey) return;
  const k = e.key.toUpperCase();
  if (k === 'D') void split('h');
  else if (k === 'E') void split('v');
  else if (k === 'W' && focused) void closePane(focused);
  else return;
  e.preventDefault();
});

setInterval(() => {
  status.textContent = panes.length + ' pane(s), ' + (panes.reduce((n, p) => n + p.bytes, 0) / 1024).toFixed(0) + ' KiB rendered';
}, 1000);

function screenText(p: Pane): string {
  const b = p.xterm.buffer.active;
  let s = '';
  for (let i = 0; i < b.length; ++i) s += (b.getLine(i)?.translateToString(true) ?? '') + '\n';
  return s;
}

function until(pred: () => boolean, ms = 15000): Promise<boolean> {
  const t0 = performance.now();
  return new Promise((resolve) => {
    const tick = (): void => {
      if (pred()) resolve(true);
      else if (performance.now() - t0 > ms) resolve(false);
      else setTimeout(tick, 20);
    };
    tick();
  });
}

// #selftest: two panes (cmd + PowerShell), echo through both, hidden-pane replay, then report to the host.
async function selftest(): Promise<void> {
  const log: string[] = [];
  let ok = true;
  const check = (cond: boolean, what: string): void => {
    log.push((cond ? 'ok ' : 'FAIL ') + what);
    ok = ok && cond;
  };
  try {
    const a = await split('h');
    const b = await split('h', 'powershell.exe -NoLogo -NoProfile');
    check(panes.length === 2, 'two panes');
    a.term.write('echo boson-pane-a\r');
    check(await until(() => screenText(a).includes('boson-pane-a\n') || /boson-pane-a\s*\n.*>/s.test(screenText(a))), 'cmd pane echo');
    b.term.write("Write-Output ('boson-' + 'pane-b')\r");
    check(await until(() => screenText(b).includes('boson-pane-b')), 'powershell pane echo');
    setHidden(a, true);
    await new Promise((r) => setTimeout(r, 100));
    const before = a.bytes;
    a.term.write('for /L %i in (1,1,200) do @echo hidden-line-%i\r');
    await new Promise((r) => setTimeout(r, 1500));
    check(a.bytes === before, 'hidden pane receives nothing (' + (a.bytes - before) + ' bytes)');
    setHidden(a, false);
    check(await until(() => screenText(a).includes('hidden-line-200')), 'replay on show');
    check(a.renderer === 'webgl' && b.renderer === 'webgl', 'renderer ' + a.renderer + '/' + b.renderer);
    const s = await a.term.stats();
    log.push('stats: sent ' + s.bytes_sent + ' acked ' + s.bytes_acked + ' replays ' + s.replays);
    await new Promise((r) => setTimeout(r, 300)); // let the WebGL frame settle for the capture
  } catch (e) {
    check(false, 'exception ' + String(e));
  }
  (window as unknown as { chrome: { webview: { postMessage(m: unknown): void } } }).chrome.webview.postMessage({ demo_selftest: { ok, log } });
}

if (location.hash === '#selftest') {
  void selftest();
} else {
  void split('h').then(() => split('h', 'powershell.exe -NoLogo'));
}
