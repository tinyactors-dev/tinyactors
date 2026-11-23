// The built extension in Chromium (node:test and Playwright): loaded
// unpacked from dist/extension (mise run build first), its side panel page
// opened as a tab bound to a fixture page's tab (?tab=<id>), Gemini
// answered by a route. Select, annotate, ask, preview, keep, reload, export.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { chromium, serve, origin, routeGemini, selectText, ROOT, FIXTURE, REWRITE } from './platform.mjs';

const EXTENSION = join(ROOT, 'dist/extension');
const ORIGINAL = 'A tiny, embeddable actor runtime for building reliable systems.';

let server;
let context;
let profile;
let extensionID;
let gemini;

before(async () => {
  server = await serve({ '/page/': FIXTURE.replace(/page\.html$/, '') });
  profile = await mkdtemp(join(tmpdir(), 'copywriter-extension-'));
  // Extensions need Chromium itself (not the headless shell): the channel
  // 'chromium' runs it in the new headless mode.
  context = await chromium.launchPersistentContext(profile, {
    channel: 'chromium',
    headless: true,
    acceptDownloads: true,
    args: [`--disable-extensions-except=${EXTENSION}`, `--load-extension=${EXTENSION}`],
  });
  let [worker] = context.serviceWorkers();
  worker ??= await context.waitForEvent('serviceworker');
  extensionID = new URL(worker.url()).host;
  gemini = await routeGemini(context);
});

after(async () => {
  await context?.close();
  server?.close();
  if (profile) await rm(profile, { recursive: true, force: true });
});

test('the manifest: a side panel, wasm allowed, no more permissions than needed', async () => {
  const page = await context.newPage();
  await page.goto(`chrome-extension://${extensionID}/manifest.json`);
  const manifest = JSON.parse(await page.locator('body').innerText());
  assert.equal(manifest.name, 'tinyactors copywriter');
  assert.equal(manifest.side_panel.default_path, 'sidepanel.html');
  assert.match(manifest.content_security_policy.extension_pages, /'wasm-unsafe-eval'/);
  assert.deepEqual(manifest.permissions.sort(), ['clipboardWrite', 'scripting', 'sidePanel', 'storage']);
  await page.close();
});

test('select, annotate, ask, preview, keep; a reload applies it again; export', async () => {
  const site = await context.newPage();
  const errors = [];
  site.on('pageerror', (e) => errors.push(`page: ${e.message}`));
  await site.goto(`${origin(server)}/page/page.html`);

  const panelPage = await context.newPage();
  panelPage.on('pageerror', (e) => errors.push(`panel: ${e.message}`));
  panelPage.on('console', (m) => { if (m.type() === 'error') errors.push(`panel: ${m.text()}`); });
  await panelPage.goto(`chrome-extension://${extensionID}/sidepanel.html`);
  const tabID = await panelPage.evaluate(async (url) => (await chrome.tabs.query({ url }))[0].id, `${origin(server)}/page/page.html`);
  await panelPage.goto(`chrome-extension://${extensionID}/sidepanel.html?tab=${tabID}`);
  const panel = panelPage.locator('.cw-panel');
  await panel.waitFor();
  await panelPage.waitForFunction(() => globalThis.reviewer?.getState().status === 'ready');
  assert.equal(await panelPage.evaluate(() => reviewer.getState().page.url), `${origin(server)}/page/page.html`);

  // The key, through Settings.
  await panelPage.locator('[data-action=settings]').click();
  await panelPage.locator('input.cw-key').fill('AIza-test-key');
  await panelPage.locator('[data-action=save-key]').click();
  await panelPage.waitForFunction(() => reviewer.getState().settings.hasKey);
  assert.deepEqual(await panelPage.evaluate(() => chrome.storage.session.get('geminiKey')), { geminiKey: 'AIza-test-key' });
  await panelPage.locator('[data-action=back]').first().click();

  // A selection in the page reaches the panel.
  await selectText(site, '#lede', ORIGINAL);
  const banner = panelPage.locator('[data-banner=selection]');
  await banner.waitFor();
  assert.match(await banner.textContent(), /embeddable actor runtime/);
  await banner.locator('[data-action=annotate]').click();
  await panelPage.locator('[data-view=detail]').waitFor();

  const composer = panelPage.locator('textarea[aria-label=Message]');
  await composer.fill('Say what a developer gets');
  await composer.press('Enter');
  const card = panelPage.locator('.cw-card', { hasText: REWRITE });
  await card.waitFor();
  assert.ok(gemini.some((url) => url.includes(':generateContent')));

  await card.locator('[data-action=preview]').click();
  await site.waitForFunction((text) => document.querySelector('#lede').textContent.includes(text), REWRITE);
  assert.equal(await site.locator('#lede').textContent(), `${REWRITE} Get started in minutes.`);
  await panelPage.locator('[data-action=keep]').click();
  await panelPage.waitForFunction(() => reviewer.getState().annotations[0]?.state === 'kept');

  // Reloaded, the page shows the kept change again.
  await site.reload();
  await site.waitForFunction((text) => document.querySelector('#lede').textContent.includes(text), REWRITE, { timeout: 10_000 });
  await panelPage.waitForFunction(() => reviewer.getState().annotations.length === 1 && reviewer.getState().annotations[0].state === 'kept');

  // Export: a ZIP download, and CHANGES.md on the clipboard.
  await panelPage.locator('[data-menu^=export]').first().click();
  const [download] = await Promise.all([
    panelPage.waitForEvent('download'),
    panelPage.locator('[data-action=export-download]').click(),
  ]);
  assert.match(download.suggestedFilename(), /^copywriter-packet-.*\.zip$/);
  const zip = await download.createReadStream().then(async (stream) => {
    const chunks = [];
    for await (const chunk of stream) chunks.push(chunk);
    return Buffer.concat(chunks);
  });
  assert.equal(zip.readUInt32LE(0), 0x04034b50, 'a ZIP');
  assert.ok(zip.includes(Buffer.from('packet.xml')));
  // What the panel writes to the clipboard (reading it back needs a
  // permission the extension does not ask for).
  await panelPage.evaluate(() => {
    const write = navigator.clipboard.writeText.bind(navigator.clipboard);
    navigator.clipboard.writeText = (text) => { globalThis.copied = text; return write(text); };
  });
  await panelPage.locator('[data-menu^=export]').first().click();
  await panelPage.locator('[data-action=export-clipboard]').click();
  await panelPage.waitForFunction(() => reviewer.getState().exporting === 'done');
  const copied = await panelPage.evaluate(() => globalThis.copied);
  assert.ok(copied.includes(REWRITE) && copied.includes(ORIGINAL), 'CHANGES.md on the clipboard');

  // Undo puts the original back in the page.
  await panelPage.evaluate((id) => reviewer.dispatch({ type: 'undo', id }), await panelPage.evaluate(() => reviewer.getState().annotations[0].id));
  await site.waitForFunction((text) => document.querySelector('#lede').textContent.startsWith(text), ORIGINAL);

  assert.ok(!(await panelPage.evaluate(() => JSON.stringify(reviewer.getState()))).includes('AIza-test-key'), 'the key is not in the state');
  assert.deepEqual(errors, []);
  await site.close();
  await panelPage.close();
});

test('a tab opened before the extension gets the content script injected; injecting twice is harmless', async () => {
  const site = await context.newPage();
  await site.goto(`${origin(server)}/page/page.html`);
  const panelPage = await context.newPage();
  await panelPage.goto(`chrome-extension://${extensionID}/sidepanel.html?tab=0`);
  const tabID = await panelPage.evaluate(async (url) => (await chrome.tabs.query({ url }))[0].id, `${origin(server)}/page/page.html`);
  const result = await panelPage.evaluate(async (id) => {
    for (let i = 0; i < 2; i++) await chrome.scripting.executeScript({ target: { tabId: id }, files: ['content.js'] });
    return chrome.tabs.sendMessage(id, { cw: 'page', method: 'info', args: [] });
  }, tabID);
  assert.equal(result.value.title, 'Fixture: actors as a library');
  await site.close();
  await panelPage.close();
});
