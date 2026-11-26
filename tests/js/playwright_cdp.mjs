// Optional Playwright check of boson::webview::CdpServer (run by boson_webview_element_tests).
//   node tests/js/playwright_cdp.mjs <playwright-core dir> <cdp http endpoint> <loopback origin>
// playwright-core is NOT a Boson dependency: install it in a folder outside the repository
// (npm install playwright-core) and point BOSON_PLAYWRIGHT_DIR at that folder.
// Prints one "PW <operation> ok|FAIL <detail>" line per operation and exits non-zero when a core one fails.
import { pathToFileURL } from 'node:url';
import { join } from 'node:path';

const [dir, endpoint, origin] = process.argv.slice(2);
if (!dir || !endpoint || !origin) {
  console.error('usage: playwright_cdp.mjs <playwright-core dir> <endpoint> <origin>');
  process.exit(2);
}
const pw = await import(pathToFileURL(join(dir, 'node_modules', 'playwright-core', 'index.mjs')).href);
const { chromium } = pw;

let coreFailures = 0;
async function op(name, fn, core = true) {
  const t0 = performance.now();
  try {
    let timer;
    const detail = await Promise.race([
      fn(),
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('timed out after 20 s')), 20000);
      }),
    ]).finally(() => clearTimeout(timer));
    console.log('PW ' + name + ' ok ' + (performance.now() - t0).toFixed(1) + 'ms' + (detail === undefined ? '' : ' ' + String(detail).slice(0, 160)));
    return true;
  } catch (err) {
    if (core) {
      coreFailures++;
    }
    console.log('PW ' + name + ' FAIL ' + String(err && err.message ? err.message : err).split('\n')[0].slice(0, 240));
    return false;
  }
}

let browser;
let context;
let page;
const timeout = 15000;
await op('connectOverCDP', async () => {
  browser = await chromium.connectOverCDP(endpoint, { timeout });
  return browser.version();
});
if (!browser) {
  process.exit(1);
}
await op('contexts/pages', async () => {
  context = browser.contexts()[0];
  if (!context) throw new Error('no default context');
  page = context.pages().find((p) => p.url().includes('/pw')) ?? context.pages()[0];
  if (!page) throw new Error('no pages');
  return context.pages().length + ' page(s): ' + page.url();
});
if (page) {
  page.setDefaultTimeout(timeout);
  await op('page.title', async () => page.title());
  await op('page.evaluate', async () => {
    const v = await page.evaluate(() => 6 * 7);
    if (v !== 42) throw new Error('got ' + v);
    return v;
  });
  await op('page.goto', async () => {
    const res = await page.goto(origin + '/pw', { waitUntil: 'load' });
    return res ? res.status() : 'no response';
  });
  await op('locator.textContent', async () => {
    const t = await page.locator('#h').textContent();
    if (t !== 'Hello Playwright') throw new Error('got ' + t);
    return t;
  });
  await op('page.fill/click', async () => {
    await page.fill('#name', 'Boson');
    await page.click('#go');
    const out = await page.locator('#out').textContent();
    if (out !== 'hi Boson') throw new Error('got ' + out);
    return out;
  });
  await op('keyboard.type', async () => {
    await page.focus('#name');
    await page.keyboard.press('End');
    await page.keyboard.type('!');
    return page.inputValue('#name');
  }, false);
  await op('page.screenshot', async () => {
    const png = await page.screenshot();
    if (png.length < 100) throw new Error('tiny screenshot');
    return png.length + ' bytes';
  });
  await op('page.on(console)', async () => {
    const got = new Promise((resolve) => page.once('console', (m) => resolve(m.text())));
    await page.evaluate(() => console.log('from-guest'));
    const text = await Promise.race([got, new Promise((_, rej) => setTimeout(() => rej(new Error('no console event')), 5000))]);
    if (text !== 'from-guest') throw new Error('got ' + text);
    return text;
  }, false);
  await op('route (Fetch domain)', async () => {
    await page.route('**/routed', (route) => route.fulfill({ status: 200, contentType: 'text/html', body: '<title>Routed</title>' }));
    await page.goto(origin + '/routed');
    const t = await page.title();
    await page.unroute('**/routed');
    if (t !== 'Routed') throw new Error('got ' + t);
    return t;
  }, false);
}
let created;
await op('context.newPage (Target.createTarget)', async () => {
  created = await context.newPage();
  await created.goto(origin + '/page/Second');
  return created.title();
}, false);
if (created) {
  await op('page.close', async () => created.close(), false);
}
await op('browser.newContext (isolated)', async () => {
  const ctx = await browser.newContext();
  const p = await ctx.newPage();
  await p.goto(origin + '/page/Isolated');
  const t = await p.title();
  await ctx.close();
  return t;
}, false);
await op('browser.close (disconnect)', async () => browser.close());
process.exit(coreFailures === 0 ? 0 : 1);
