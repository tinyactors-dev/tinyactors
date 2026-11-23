// The built widget in Chromium (node:test and Playwright): the fixture page
// with the website's loader, dist/site served at /copywriter/ (mise run
// build first), a phone's viewport, Gemini answered by a route. Without the
// opt-in nothing loads; with it: select, open the sheet (the tap clears the
// selection, the widget keeps it), annotate, ask, preview, keep, reload,
// export.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { dirname, join } from 'node:path';
import { chromium, serve, origin, routeGemini, selectText, ROOT, FIXTURE, REWRITE } from './platform.mjs';

const ORIGINAL = 'A tiny, embeddable actor runtime for building reliable systems.';
const PHONE = { viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true };

let server;
let browser;

before(async () => {
  server = await serve({ '/page/': dirname(FIXTURE), '/copywriter/': join(ROOT, 'dist/site') });
  browser = await chromium.launch();
});

after(async () => {
  await browser?.close();
  server?.close();
});

const scripts = (page) => page.evaluate(() => performance.getEntriesByType('resource').map((e) => e.name).filter((n) => n.includes('/copywriter/')));

test('without the opt-in, the page loads nothing of the copywriter', async () => {
  const context = await browser.newContext(PHONE);
  const page = await context.newPage();
  await page.goto(`${origin(server)}/page/page.html`);
  await page.waitForLoadState('networkidle');
  assert.deepEqual(await scripts(page), []);
  assert.equal(await page.locator('#tinyactors-copywriter').count(), 0);
  await context.close();
});

test('the /copywriter/ page turns the widget on and off', async () => {
  const context = await browser.newContext(PHONE);
  const page = await context.newPage();
  await page.goto(`${origin(server)}/copywriter/`);
  assert.equal(await page.locator('meta[name=robots]').getAttribute('content'), 'noindex, nofollow');
  assert.equal(await page.locator('a[data-download]').getAttribute('href'), 'copywriter-extension.zip');
  const response = await page.request.get(`${origin(server)}/copywriter/copywriter-extension.zip`);
  assert.equal((await response.body()).readUInt32LE(0), 0x04034b50);
  await page.locator('label.switch').click();
  assert.equal(await page.evaluate(() => localStorage.getItem('tinyactors-copywriter')), 'on');
  await page.locator('label.switch').click();
  assert.equal(await page.evaluate(() => localStorage.getItem('tinyactors-copywriter')), 'off');
  await context.close();
});

test('on a phone: select, open, annotate, ask, preview, keep; a reload applies it again; export', async () => {
  const context = await browser.newContext({ ...PHONE, acceptDownloads: true });
  await context.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: origin(server) });
  await context.addInitScript(() => {
    localStorage.setItem('tinyactors-copywriter', 'on');
    sessionStorage.setItem('tinyactors-copywriter-key', 'AIza-test-key');
  });
  const gemini = await routeGemini(context);
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(`${origin(server)}/page/page.html`);
  const launcher = page.locator('#tinyactors-copywriter .launcher');
  await launcher.waitFor();
  await page.waitForFunction(() => globalThis.tinyactorsCopywriter?.reviewer.getState().status === 'ready');

  // A selection, then a tap on the launcher; the tap clears the page's
  // selection, as a phone does, and the widget keeps the last one.
  await selectText(page, '#lede', ORIGINAL);
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection);
  await launcher.tap();
  await page.evaluate(() => getSelection().removeAllRanges());
  const sheet = page.locator('#tinyactors-copywriter .sheet');
  await sheet.locator('.cw-panel').waitFor();
  const box = await sheet.boundingBox();
  assert.ok(Math.abs(box.height - 844 * 0.85) < 2, `the sheet is 85% of the viewport: ${box.height}`);
  const banner = sheet.locator('[data-banner=selection]');
  await banner.waitFor();
  await page.waitForTimeout(300);  // past the selection's debounce
  assert.equal(await banner.count(), 1, 'the selection is kept after the tap');
  await banner.locator('[data-action=annotate]').tap();
  await sheet.locator('[data-view=detail]').waitFor();

  const composer = sheet.locator('textarea[aria-label=Message]');
  await composer.fill('Say what a developer gets');
  await composer.press('Enter');
  const card = sheet.locator('.cw-card', { hasText: REWRITE });
  await card.waitFor();
  assert.ok(gemini.some((url) => url.includes(':generateContent')));
  await card.locator('[data-action=preview]').tap();
  await page.waitForFunction((text) => document.querySelector('#lede').textContent.includes(text), REWRITE);
  await sheet.locator('[data-action=keep]').tap();
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().annotations[0]?.state === 'kept');

  // Export to the clipboard and as a download.
  await sheet.locator('[data-menu^=export]').first().tap();
  await sheet.locator('[data-action=export-clipboard]').tap();
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().exporting === 'done');
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  assert.ok(copied.includes(REWRITE) && copied.includes(ORIGINAL));
  await sheet.locator('[data-menu^=export]').first().tap();
  const [download] = await Promise.all([page.waitForEvent('download'), sheet.locator('[data-action=export-download]').tap()]);
  assert.match(download.suggestedFilename(), /\.zip$/);

  // Reloaded: the change is applied again, and the sheet stays open.
  await page.reload();
  await page.waitForFunction((text) => document.querySelector('#lede')?.textContent.includes(text), REWRITE, { timeout: 10_000 });
  await page.locator('#tinyactors-copywriter .sheet .cw-panel').waitFor();

  // The grip closes the sheet: dragged down past half its height.
  const grip = await page.locator('#tinyactors-copywriter .grip').boundingBox();
  await page.mouse.move(grip.x + grip.width / 2, grip.y + 10);
  await page.mouse.down();
  await page.mouse.move(grip.x + grip.width / 2, grip.y + 600, { steps: 8 });
  await page.mouse.up();
  await page.waitForFunction(() => !document.getElementById('tinyactors-copywriter').hasAttribute('data-open'));
  assert.deepEqual(errors, []);
  await context.close();
});

test('from 900px wide, the panel is a drawer on the right', async () => {
  const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  await context.addInitScript(() => localStorage.setItem('tinyactors-copywriter', 'on'));
  const page = await context.newPage();
  await page.goto(`${origin(server)}/page/page.html`);
  await page.locator('#tinyactors-copywriter .launcher').click();
  const sheet = page.locator('#tinyactors-copywriter .sheet');
  await sheet.locator('.cw-panel').waitFor();
  await page.waitForTimeout(400);
  const box = await sheet.boundingBox();
  assert.deepEqual([box.x, box.y, box.width, box.height], [880, 0, 400, 800]);
  await page.locator('#tinyactors-copywriter .close').click();
  await page.waitForFunction(() => !document.getElementById('tinyactors-copywriter').hasAttribute('data-open'));
  await context.close();
});
