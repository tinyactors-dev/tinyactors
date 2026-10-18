// The module in Chromium (the pre-installed Playwright's, /opt/pw-browsers,
// as staticjs-js's test finds it): a page and a module Worker import both
// entries over HTTP (index.mjs fetches tinyactors.wasm by its URL with
// top-level await; init.mjs's init() answers the same instance), make a
// host, run a null-datamodel chart and free it, show a throwing import
// leaves no stack behind and a trap poisons only its own instance. Where
// no Playwright is installed it says so and passes: the test task
// requires one (mise/tasks/wasm/test).
import assert from 'node:assert/strict';
import { run, test } from '../../test/harness.mjs';
import { launch, napi, playwright, serve } from './serve.mjs';

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
  const server = await serve(new Map([[napi('page.html'), '<!doctype html><title>tinyactors</title>']]));
  const browser = await launch(pw);
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', (error) => errors.push(error));
    await page.goto(`http://127.0.0.1:${server.address().port}${napi('page.html')}`);
    check(await page.evaluate(async (url) => (await import(url)).everything(), napi('wasm/test/browser.mjs')), 'page');
    const worker = await page.evaluate((url) => new Promise((resolve) => {
      const w = new Worker(url, { type: 'module' });
      w.onmessage = (event) => resolve(event.data);
      w.onerror = (event) => resolve({ error: event.message });
    }), napi('wasm/test/worker.mjs'));
    assert.equal(worker.error, undefined, worker.error);
    check(worker.out, 'worker');
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
    server.closeAllConnections();
    server.close();
  }
});

await run();
