// The module in Chromium (the pre-installed Playwright's, /opt/pw-browsers,
// as staticjs-js's test finds it): a page and a module Worker import both
// entries over HTTP (index.mjs fetches tinyactors.wasm by its URL with
// top-level await; init.mjs's init() answers the same instance), make a
// host, run a null-datamodel chart and free it, show a throwing import
// leaves no stack behind and a trap poisons only its own instance. Where
// no Playwright is installed it says so and passes: the test task
// requires one (mise/tasks/wasm/test).
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { run, test } from '../../test/harness.mjs';

const root = fileURLToPath(new URL('..', import.meta.url));
const TYPES = { '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html' };

function playwright() {
  const require = createRequire(import.meta.url);
  const paths = [root];
  try {
    paths.push(execFileSync('npm', ['root', '-g'], { encoding: 'utf8' }).trim());
  } catch {
    // no npm on PATH
  }
  try {
    return require(require.resolve('playwright', { paths }));
  } catch {
    return null;
  }
}

function serve() {
  const server = createServer(async (request, response) => {
    const path = normalize(join(root, decodeURIComponent(new URL(request.url, 'http://x').pathname)));
    if (!path.startsWith(root)) return response.writeHead(403).end();
    if (path === join(root, 'page.html')) return response.writeHead(200, { 'content-type': 'text/html' }).end('<!doctype html><title>tinyactors</title>');
    let body;
    try {
      body = await readFile(path);
    } catch {
      return response.writeHead(404).end();
    }
    return response.writeHead(200, { 'content-type': TYPES[extname(path)] ?? 'application/octet-stream' }).end(body);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

function check(out, where) {
  assert.deepEqual(out.entries, ['function', 'function', 'TA_ERROR_STATE'], where);
  assert.deepEqual(out.nullChart.sends, [{ event: 'hello', target: 'world', processor: 1, session: '1n' }], where);
  assert.equal(out.nullChart.freed, 0, where);
  assert.equal(out.throwing.raised, 200, where);
  assert.equal(out.throwing.after, out.throwing.before, `${where}: the stack is where it was`);
  assert.equal(out.throwing.envDepth, 0, where);
  assert.deepEqual(out.forwarding.logs, [{ label: 'it', text: 'greeting says hello' }], where);
  assert.equal(out.trap.poisoned, true, where);
  for (const seen of out.trap.seen) assert.deepEqual([seen.status, seen.cause], ['TA_ERROR_STATE', 'RuntimeError'], where);
  assert.equal(out.afterTrap, 1, `${where}: the entries' instance is not the poisoned one`);
}

test('the module in a Chromium page and a module Worker, through both entries', async () => {
  const pw = playwright();
  if (pw === null) {
    console.log('skip: no Playwright installed');
    return;
  }
  const server = await serve();
  const executablePath = process.env.PLAYWRIGHT_CHROMIUM;
  const browser = await pw.chromium.launch(executablePath ? { executablePath } : {});
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', (error) => errors.push(error));
    await page.goto(`http://127.0.0.1:${server.address().port}/page.html`);
    check(await page.evaluate(async () => (await import('/test/browser.mjs')).everything()), 'page');
    const worker = await page.evaluate(() => new Promise((resolve) => {
      const w = new Worker('/test/worker.mjs', { type: 'module' });
      w.onmessage = (event) => resolve(event.data);
      w.onerror = (event) => resolve({ error: event.message });
    }));
    assert.equal(worker.error, undefined, worker.error);
    check(worker.out, 'worker');
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    server.close();
  }
});

await run();
