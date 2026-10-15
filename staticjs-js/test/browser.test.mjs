// @tinyactors/staticjs in a browser (Chromium, through Playwright): load()
// fetches the bundled staticjs.wasm by its URL, a page evaluates through
// the package, and runs on slices of fuel take turns with the page.
// Skipped, with a note, where no Playwright is installed.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));

// Playwright from this package's node_modules, else the one installed
// globally (as on the project's CI image), else none.
function playwright() {
  const require = createRequire(import.meta.url);
  const paths = [root];
  try {
    paths.push(execFileSync('npm', ['root', '-g'], { encoding: 'utf8' }).trim());
  } catch {
    // No npm on PATH: only the package's own.
  }
  try {
    return require(require.resolve('playwright', { paths }));
  } catch {
    return null;
  }
}

const TYPES = { '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html' };

function serve() {
  const server = createServer(async (request, response) => {
    const path = normalize(join(root, decodeURIComponent(new URL(request.url, 'http://x').pathname)));
    if (!path.startsWith(root)) return response.writeHead(403).end();
    if (path === join(root, 'page.html')) {
      return response.writeHead(200, { 'content-type': 'text/html' }).end('<!doctype html><title>staticjs</title>');
    }
    try {
      const body = await readFile(path);
      response.writeHead(200, { 'content-type': TYPES[extname(path)] ?? 'application/octet-stream' }).end(body);
    } catch {
      response.writeHead(404).end();
    }
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

test('the package in Chromium', async (t) => {
  const pw = playwright();
  if (pw === null) {
    t.skip('no Playwright installed');
    return;
  }
  const server = await serve();
  const browser = await pw.chromium.launch(process.env.PLAYWRIGHT_CHROMIUM ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM } : {});
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', (error) => errors.push(error));
    await page.goto(`http://127.0.0.1:${server.address().port}/page.html`);
    const out = await page.evaluate(async () => {
      const { load } = await import('/index.mjs');
      const staticjs = await load();
      const context = staticjs.createContext();
      const program = context.compile('rows.filter((r) => r.total > limit).map((r) => [r.id, r.total * 2n])',
        { globals: ['rows', 'limit'] });
      const run = program.start({ rows: [{ id: 'a', total: 5n }, { id: 'b', total: 50n }], limit: 10n });
      run.step(10_000);
      const result = run.result();
      // Slices of fuel: the page's timers run between them.
      const long = context.compile('Array.from({ length: 50000 }, (_, i) => i).reduce((a, b) => a + b, 0)');
      const slices = long.start();
      let ticks = 0;
      const timer = setInterval(() => ticks++, 0);
      let turns = 0;
      while (!slices.step(20_000)) {
        turns++;
        await new Promise((resolve) => setTimeout(resolve, 1));
      }
      clearInterval(timer);
      const sum = slices.result();
      for (const x of [run, slices, program, long]) x.dispose();
      context.dispose();
      return { version: staticjs.version, result, sum, turns, ticks };
    });
    assert.deepEqual(out.result, [['b', 100n]]);
    assert.equal(out.sum, 1249975000);
    assert.ok(out.turns > 3, 'the long run took turns');
    assert.ok(out.ticks > 0, 'the page\'s timer ran between them');
    assert.match(out.version, /^\d+\.\d+\.\d+$/);
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    server.close();
  }
});
