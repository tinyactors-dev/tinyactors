// The panel against the mock reviewer, in Chromium: node test/ui-panel.spec.mjs
// (node:test and Playwright; no network: tools/serve.mjs bundles the
// preview with the pinned esbuild and serves it on a free port).
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { startPreviewServer } from '../tools/serve.mjs';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

let server;
let browser;
let base;

before(async () => {
  server = await startPreviewServer(0);
  base = `http://127.0.0.1:${server.address().port}/`;
  browser = await chromium.launch();
});

after(async () => {
  await browser?.close();
  server?.close();
});

async function open(query, options = {}) {
  const page = await browser.newPage({ viewport: { width: 400, height: 800 }, ...options });
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => { if (message.type() === 'error') errors.push(message.text()); });
  await page.goto(`${base}?bare=1&delay=0.05&${query}`);
  await page.locator('#panel .cw-panel').waitFor();
  return { page, panel: page.locator('#panel'), errors };
}

const commands = (page) => page.evaluate(() => reviewer.log.map((c) => c.type));

test('the list shows the annotations, filtered', async () => {
  const { page, panel, errors } = await open('');
  assert.equal(await panel.locator('.cw-row').count(), 3, 'open: suggested, previewing, draft');
  await panel.locator('[data-filter=kept]').click();
  assert.equal(await panel.locator('.cw-row').count(), 1);
  await panel.locator('[data-filter=all]').click();
  assert.equal(await panel.locator('.cw-row').count(), 4);
  assert.match(await panel.locator('.cw-subtitle').textContent(), /tinyactors\.dev/);
  assert.match(await panel.locator('.cw-totals').textContent(), /3 kept · 3 open · 3 pages/);
  assert.deepEqual(errors, []);
  await page.close();
});

test('annotate, ask, preview, keep, back', async () => {
  const { page, panel, errors } = await open('');
  await page.evaluate(() => reviewer.setSelection('Everything you need to build actor systems'));
  const banner = panel.locator('[data-banner=selection]');
  await banner.waitFor();
  assert.match(await banner.textContent(), /Everything you need/);
  await banner.locator('[data-action=annotate]').click();

  const detail = panel.locator('[data-view=detail]');
  await detail.waitFor();
  assert.match(await detail.locator('.cw-original').textContent(), /Everything you need/);
  assert.equal(await banner.count(), 0, 'the selection was used');

  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('Lead with what a developer gets');
  await composer.press('Enter');
  assert.equal(await composer.inputValue(), '');
  await panel.locator('[data-thinking], .cw-thinking').first().waitFor();
  await detail.locator('.cw-card').first().waitFor();
  assert.ok(await detail.locator('.cw-msg--user').count() >= 1);
  assert.ok(await detail.locator('.cw-msg--model strong').count() >= 1, 'markdown bold');

  await detail.locator('[data-action=preview]').first().click();
  await detail.locator('.cw-card.is-chosen [data-action=keep]').waitFor();
  assert.equal(await detail.locator('.cw-original.is-replaced').count(), 1);
  await detail.locator('[data-action=keep]').click();
  await panel.locator('.cw-badge[data-state=kept]').first().waitFor();

  await page.keyboard.press('Escape');
  await panel.locator('[data-view=list]').waitFor();
  await panel.locator('[data-filter=kept]').click();
  assert.equal(await panel.locator('.cw-row').count(), 2);
  assert.deepEqual(
    (await commands(page)).filter((t) => t !== 'select'),
    ['annotate', 'ask', 'preview', 'keep', 'deselect'],
  );
  assert.deepEqual(errors, []);
  await page.close();
});

test('write your own: the edit is previewed', async () => {
  const { page, panel } = await open('');
  await panel.locator('.cw-row[data-id=a4]').click();
  await panel.locator('[data-action=write]').click();
  const area = panel.locator('.cw-edit textarea');
  assert.equal(await area.evaluate((node) => node === node.getRootNode().activeElement), true, 'the editor has focus');
  await area.fill('Four million actors in 1.5 GB');
  await panel.locator('[data-action=edit-preview]').click();
  await panel.locator('.cw-card.is-chosen').waitFor();
  assert.match(await panel.locator('.cw-card.is-chosen').textContent(), /Four million actors.*Your version/);
  await page.close();
});

test('the composer keeps its text and focus across state updates', async () => {
  const { page, panel } = await open('');
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('half a thought');
  await page.evaluate(() => {
    reviewer.setSelection('Get started in minutes');
    reviewer.set({ annotations: reviewer.getState().annotations.slice(1) });
    reviewer.setSelection(null);
  });
  await page.waitForTimeout(50);
  assert.equal(await composer.inputValue(), 'half a thought');
  assert.equal(await composer.evaluate((node) => node === node.getRootNode().activeElement), true);
  await page.close();
});

test('first run: no key leads to settings; saving one, the model and Escape', async () => {
  const { page, panel } = await open('key=0');
  assert.equal(await panel.locator('[data-notice=first-run]').count(), 1);
  assert.equal(await panel.locator('[data-action=send]').isDisabled(), true);
  await panel.locator('[data-action=add-key]').click();
  await panel.locator('[data-view=settings]').waitFor();
  const key = panel.locator('input.cw-key');
  assert.equal(await key.getAttribute('type'), 'password');
  await key.fill('AIza-test-key');
  await panel.locator('[aria-label="Show key"]').click();
  assert.equal(await key.getAttribute('type'), 'text');
  assert.equal(await key.inputValue(), 'AIza-test-key', 'the draft survives the rebuild');
  assert.equal(await panel.locator('a[data-action=get-key]').getAttribute('href'), 'https://aistudio.google.com/apikey');
  await panel.locator('[data-action=save-key]').click();
  await panel.locator('.cw-group-title .cw-badge', { hasText: 'Saved' }).waitFor();
  await panel.locator('.cw-toast', { hasText: 'Key saved' }).waitFor();
  assert.equal(await key.inputValue(), '');

  await panel.locator('#cw-model').selectOption('gemini-3.8-flash');
  await page.waitForFunction(() => reviewer.getState().settings.model === 'gemini-3.8-flash');
  await panel.locator('[data-action=refresh-models]').click();
  await page.waitForFunction(() => reviewer.getState().settings.models.length === 4);

  const log = await page.evaluate(() => reviewer.log);
  assert.equal(log.find((c) => c.type === 'settings.save' && c.apiKey).apiKey, 'AIza-test-key');
  await page.keyboard.press('Escape');
  await panel.locator('[data-view=list]').waitFor();
  assert.equal(await panel.locator('[data-notice=first-run]').count(), 0);
  await page.close();
});

test('export: the menu, a download and a refusal', async () => {
  const { page, panel } = await open('');
  await panel.locator('[data-menu=export-foot]').click();
  const menu = panel.locator('.cw-menu[role=menu]');
  await menu.waitFor();
  await menu.locator('[data-scope=all]').click();
  await menu.locator('[data-action=export-download]').click();
  await panel.locator('.cw-toast', { hasText: 'Work packet downloaded' }).waitFor();
  assert.deepEqual((await page.evaluate(() => reviewer.log)).at(-1), { type: 'export', scope: 'all', to: 'download' });

  await page.evaluate(() => reviewer.set({ totals: { kept: 0, open: 1, pages: 1 } }));
  await panel.locator('[data-menu=export-head]').click();
  await page.keyboard.press('Escape');
  assert.equal(await menu.count(), 0, 'Escape closes the menu');
  await panel.locator('[data-menu=export-head]').click();
  await panel.locator('[data-action=export-clipboard]').click();
  await panel.locator('.cw-toast--error', { hasText: 'Nothing is kept yet' }).waitFor();
  await page.close();
});

test('page chat: a message, the answer, a proposed annotation', async () => {
  const { page, panel } = await open('');
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('What should change?');
  await composer.press('Enter');
  await panel.locator('[data-view=chat] .cw-msg--model').waitFor();
  assert.equal(await panel.locator('[data-tab=chat]').getAttribute('aria-selected'), 'true');
  assert.match(await panel.locator('[data-tab=notes] .cw-count').textContent(), /5/);
  await page.close();
});

test('push-to-talk: hold records until release, a tap toggles', async () => {
  const { page, panel } = await open('');
  const mic = panel.locator('[data-action=mic]');
  const box = await mic.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.waitForTimeout(450);
  assert.equal(await mic.getAttribute('aria-pressed'), 'true');
  await page.mouse.up();
  await page.waitForFunction(() => reviewer.log.some((c) => c.type === 'record.stop'));
  await page.mouse.down();
  await page.mouse.up();
  await page.waitForTimeout(50);
  assert.equal(await mic.getAttribute('aria-pressed'), 'true', 'a tap keeps recording');
  await page.mouse.down();
  await page.mouse.up();
  const log = (await page.evaluate(() => reviewer.log)).filter((c) => c.type.startsWith('record.'));
  assert.deepEqual(log.map((c) => c.type), ['record.start', 'record.stop', 'record.start', 'record.stop']);
  assert.equal(log[0].target, 'page');
  await page.close();
});

test('sheet: tap targets of 44px, light and dark', async () => {
  for (const colorScheme of ['light', 'dark']) {
    const { page, panel, errors } = await open('layout=sheet', { viewport: { width: 390, height: 844 }, colorScheme, hasTouch: true });
    await page.evaluate(() => reviewer.setSelection('Everything you need to build actor systems'));
    await panel.locator('[data-banner=selection]').waitFor();
    const small = await panel.locator('.cw-panel').evaluate((root) => [...root.querySelectorAll('button')]
      .filter((b) => b.offsetParent && !b.closest('.cw-segmented, .cw-toast'))
      .map((b) => [b.getAttribute('aria-label') ?? b.textContent.trim(), b.getBoundingClientRect().height])
      .filter(([, height]) => height < 43.5));
    assert.deepEqual(small, []);
    const background = await panel.locator('.cw-panel').evaluate((node) => getComputedStyle(node).backgroundColor);
    assert.equal(background, colorScheme === 'dark' ? 'rgb(32, 24, 17)' : 'rgb(250, 246, 238)');
    const banner = await panel.locator('[data-banner=selection]').boundingBox();
    assert.ok(banner.y > 844 * 0.6, 'the Annotate button is in thumb reach');
    assert.deepEqual(errors, []);
    await page.close();
  }
});

test('unmount removes the panel and its stylesheet', async () => {
  const { page, panel } = await open('');
  await page.evaluate(() => panel.unmount());
  assert.equal(await panel.locator('.cw-panel').count(), 0);
  assert.equal(await panel.locator('style[data-cw-panel]').count(), 0);
  await page.close();
});
