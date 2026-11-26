// Runs inside the boson_terminal_tests WebView (bundled with bun build --format=iife): drives frontend/src/terminal.ts
// and frontend/src/compat/node_pty.ts against the real TerminalHost / TerminalController and reports through
// window.__termSelftest.
import { attachRenderer, listTerminals, spawnTerminal } from '../../frontend/src/terminal';
import * as pty from '../../frontend/src/compat/node_pty';

interface SelftestState {
  done: boolean;
  ok: boolean;
  log: string[];
}

const state: SelftestState = { done: false, ok: false, log: [] };
const w = window as unknown as { __termSelftest: SelftestState; T?: { ignoreAll?: boolean } };
w.__termSelftest = state;
if (w.T) w.T.ignoreAll = true; // the C++ test page's own listener steps aside

function expect(cond: boolean, what: string): void {
  state.log.push((cond ? 'ok ' : 'FAIL ') + what);
}

function until(pred: () => boolean, timeoutMs = 15000): Promise<boolean> {
  const start = performance.now();
  return new Promise((resolve) => {
    const tick = (): void => {
      if (pred()) resolve(true);
      else if (performance.now() - start > timeoutMs) resolve(false);
      else setTimeout(tick, 10);
    };
    tick();
  });
}

async function run(): Promise<void> {
  // 1. Output produced before any renderer attached is buffered (spawned hidden) and onExit comes after it.
  {
    const t = await spawnTerminal({ command: 'cmd.exe', args: ['/d', '/c', 'echo selftest-one& exit 3'] });
    await new Promise((r) => setTimeout(r, 600)); // exits while nothing listens
    const dec = new TextDecoder();
    let text = '';
    let textAtExit = '';
    const exited = new Promise<number>((resolve) =>
      t.onExit((code) => {
        textAtExit = text;
        resolve(code);
      })
    );
    attachRenderer(t, {
      // async renderer: the chunk stays mapped until this resolves
      write: (d) => {
        text += dec.decode(d, { stream: true });
        return new Promise((r) => setTimeout(r, 2));
      },
    });
    const code = await exited;
    expect(code === 3, 'spawnTerminal exit code 3 (got ' + code + ')');
    expect(textAtExit.includes('selftest-one'), 'buffered output delivered before onExit');
    await t.dispose();
  }

  // 2. Interactive shell: fast-path input, resize, info, processes, title, kill.
  {
    const t = await spawnTerminal({ command: 'cmd.exe', args: ['/d', '/k', 'prompt ST$G'], cols: 90, rows: 25 });
    expect(t.fastInput, 'fast input path available');
    const dec = new TextDecoder();
    let out = '';
    t.onData((d) => {
      out += dec.decode(d, { stream: true });
    });
    let title = '';
    t.onTitle((s) => {
      title = s;
    });
    expect(await until(() => out.includes('ST>')), 'prompt rendered');
    t.write('echo self^test-two\r');
    expect(await until(() => out.includes('selftest-two')), 'echo through fast path');
    t.write('title selftest-title\r');
    expect(await until(() => title === 'selftest-title'), 'onTitle');
    await t.resize(100, 30);
    const info = await t.info();
    expect(info.cols === 100 && info.rows === 30 && info.running, 'resize + info');
    const procs = await t.processes();
    expect(procs.foreground_name.toLowerCase() === 'cmd.exe' && procs.processes.length >= 1, 'processes(): ' + procs.foreground_name);
    expect(procs.cwd.length > 0, 'cwd known (' + procs.cwd_source + ')');
    const list = await listTerminals();
    expect(list.some((x) => x.id === t.id), 'listTerminals');
    const stats = await t.stats();
    expect(stats.bytes_in > 0 && stats.bytes_dropped === 0, 'stats');
    const exited = new Promise<number>((resolve) => t.onExit(resolve));
    await t.kill();
    expect((await exited) === 1, 'kill -> exit code 1');
    await t.dispose();
  }

  // 3. node-pty shim: string data, exit, queued write before ready, UTF-8 decoding.
  {
    const p = pty.spawn('cmd.exe', ['/d', '/c', 'echo node-pty-ok& exit 4'], { cols: 80, rows: 24 });
    let text = '';
    p.onData((d) => {
      text += d;
    });
    const e = await new Promise<pty.IExitEvent>((resolve) => p.onExit(resolve));
    expect(e.exitCode === 4, 'node-pty exit code 4 (got ' + e.exitCode + ')');
    expect(text.includes('node-pty-ok'), 'node-pty onData');
    expect(p.pid > 0, 'node-pty pid after ready');

    const q = pty.spawn('cmd.exe', ['/d', '/k', 'prompt Q$G'], {});
    let qt = '';
    q.onData((d) => {
      qt += d;
    });
    q.write('echo [ünï ✓ 日]\r'); // queued until the PTY exists
    expect(await until(() => qt.includes('[ünï ✓ 日]')), 'node-pty UTF-8 round trip');
    expect((await q.getForegroundProcess()).toLowerCase() === 'cmd.exe', 'getForegroundProcess');
    const qe = new Promise<pty.IExitEvent>((resolve) => q.onExit(resolve));
    q.kill();
    await qe;
  }
  state.ok = state.log.every((l) => !l.startsWith('FAIL'));
}

run()
  .catch((err: unknown) => {
    state.log.push('FAIL exception ' + String(err));
  })
  .finally(() => {
    state.done = true;
  });
