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
  assert.equal(await page.locator('a[data-download]').getAttribute('href'), '/copywriter/copywriter-extension.zip');
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

  // A selection, then a tap on the launcher, which annotates it: the sheet
  // opens on the new annotation. The tap clears the page's selection, as
  // a phone does, and the widget keeps the last one.
  await selectText(page, '#lede', ORIGINAL);
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection);
  assert.equal(await launcher.getAttribute('aria-label'), 'Annotate the selection');
  await launcher.tap();
  await page.evaluate(() => getSelection().removeAllRanges());
  const sheet = page.locator('#tinyactors-copywriter .sheet');
  await sheet.locator('.cw-panel').waitFor();
  await page.waitForTimeout(400);  // the sheet slides in
  const box = await sheet.boundingBox();
  assert.ok(Math.abs(box.height - 844 * 0.4) < 2, `the sheet opens at 40% of the viewport, the page above it: ${box.height}`);
  await sheet.locator('[data-view=detail]').waitFor();
  assert.equal(await sheet.locator('.cw-original').textContent(), ORIGINAL);
  const banner = sheet.locator('[data-banner=selection]');
  assert.equal(await banner.count(), 0, 'the selection was used');

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

// A phone with the widget on and a key, Gemini routed, the reviewer ready.
async function phone() {
  const context = await browser.newContext(PHONE);
  await context.addInitScript(() => {
    localStorage.setItem('tinyactors-copywriter', 'on');
    sessionStorage.setItem('tinyactors-copywriter-key', 'AIza-test-key');
  });
  await routeGemini(context);
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`${origin(server)}/page/page.html`);
  await page.waitForFunction(() => globalThis.tinyactorsCopywriter?.reviewer.getState().status === 'ready');
  return { context, page, errors, sheet: page.locator('#tinyactors-copywriter .sheet'), launcher: page.locator('#tinyactors-copywriter .launcher') };
}

const FAR = 'The far paragraph is where a phone\'s sheet would cover a preview.';

test('on a phone, the sheet resizes to where it is let go, from the grip or the header, and slides away when pulled down', async () => {
  const { context, page, errors, sheet, launcher } = await phone();
  await launcher.tap();
  await sheet.locator('.cw-panel').waitFor();
  await page.waitForTimeout(400);
  const drag = async (selector, dy) => {
    const box = await sheet.locator(selector).first().boundingBox();
    const x = box.x + (selector === '.grip' ? box.width / 2 : 60), y = box.y + box.height / 2;
    await page.mouse.move(x, y);
    await page.mouse.down();
    await page.mouse.move(x, y + dy, { steps: 20 });
    await page.waitForTimeout(150);   // a slow drag: no flick
    await page.mouse.up();
    await page.waitForTimeout(350);
  };
  const height = async () => (await sheet.boundingBox()).height;
  const before = await height();
  await drag('.grip', -200);
  assert.ok(Math.abs(await height() - (before + 200)) < 4, `up by 200 from the grip: ${before} → ${await height()}`);
  await drag('.cw-header h1, .cw-header .cw-title, .cw-header', 120);
  assert.ok(Math.abs(await height() - (before + 80)) < 4, `down by 120 from the header: ${await height()}`);
  assert.ok(await page.locator('#tinyactors-copywriter').evaluate((n) => n.hasAttribute('data-open')), 'still open');
  // the height is kept across a reload
  const kept = await height();
  await page.reload();
  await page.locator('#tinyactors-copywriter .sheet .cw-panel').waitFor();
  await page.waitForTimeout(400);
  assert.ok(Math.abs(await height() - kept) < 4, `kept: ${kept} → ${await height()}`);
  // a slow drag far down closes it
  await drag('.grip', 700);
  assert.ok(!(await page.locator('#tinyactors-copywriter').evaluate((n) => n.hasAttribute('data-open'))), 'closed');
  assert.deepEqual(errors, []);
  await context.close();
});

test('on a phone, a conversation with a reader and rewrites side by side raise the sheet to its high size; leaving them lowers it back', async () => {
  const { context, page, errors, sheet, launcher } = await phone();
  await page.evaluate(() => tinyactorsCopywriter.reviewer.dispatch({ type: 'settings.save', audienceIDs: ['site-tech-lead'] }));
  await launcher.tap();
  await sheet.locator('.cw-panel').waitFor();
  await page.waitForTimeout(400);
  const share = async () => (await sheet.boundingBox()).height / 844;
  assert.ok(Math.abs(await share() - 0.4) < 0.02, `opens low: ${await share()}`);
  await sheet.locator('.cw-strip-chip').first().tap();
  await sheet.locator('.cw-menu [data-action=talk]').tap();
  await page.waitForTimeout(400);
  assert.ok(Math.abs(await share() - 0.75) < 0.02, `raised: ${await share()}`);
  await sheet.locator('[data-action=back]').tap();
  await page.waitForTimeout(400);
  assert.ok(Math.abs(await share() - 0.4) < 0.02, `back where it was: ${await share()}`);
  // rewrites side by side rise it to compare; a preview lowers it so the page shows
  await page.evaluate(() => tinyactorsCopywriter.reviewer.dispatch({ type: 'settings.save', audienceIDs: ['site-tech-lead', 'site-workflow-tester'] }));
  await selectText(page, '#lede', ORIGINAL);
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection);
  await page.evaluate(() => tinyactorsCopywriter.reviewer.dispatch({ type: 'annotate', note: 'Sharper' }));
  await sheet.locator('.cw-cards--compare').waitFor();
  await page.waitForTimeout(400);
  assert.ok(Math.abs(await share() - 0.75) < 0.02, `raised to compare: ${await share()}`);
  await sheet.locator('.cw-cards--compare [data-action=preview]').first().tap();
  await page.waitForTimeout(500);
  assert.ok(await share() <= 0.41, `lowered for the preview: ${await share()}`);
  assert.deepEqual(errors, []);
  await context.close();
});

test('on a phone, the launcher is bottom left and stays there while the page scrolls', async () => {
  const { context, page, launcher } = await phone();
  const before = await launcher.boundingBox();
  assert.ok(before.x < 390 / 2, `bottom left, clear of the site's menu: x ${before.x}`);
  for (const y of [200, 600, 1200]) {
    await page.mouse.wheel(0, y);
    await page.waitForTimeout(100);
    assert.ok(await launcher.isVisible(), `visible after scrolling down ${y}`);
    const box = await launcher.boundingBox();
    assert.deepEqual([Math.round(box.x), Math.round(box.y)], [Math.round(before.x), Math.round(before.y)]);
  }
  await context.close();
});

test('on a phone, a preview shows above the sheet: lowered to half height, the text scrolled into the rest', async () => {
  const { context, page, errors, sheet, launcher } = await phone();
  await launcher.tap();
  await sheet.locator('.cw-panel').waitFor();
  // a tap on the grip raises the sheet to its full height
  await sheet.locator('.grip').tap();
  await page.waitForTimeout(400);
  assert.ok(Math.abs((await sheet.boundingBox()).height - 844 * 0.75) < 2, 'full height');
  // the paragraph sits behind the full sheet
  await page.evaluate(() => document.querySelector('#far').scrollIntoView({ block: 'end' }));
  await selectText(page, '#far', FAR);
  const annotate = sheet.locator('[data-banner=selection] [data-action=annotate]');
  await annotate.waitFor();
  await annotate.tap();
  const composer = sheet.locator('textarea[aria-label=Message]');
  await composer.fill('Shorter');
  await composer.press('Enter');
  const card = sheet.locator('.cw-card', { hasText: REWRITE });
  await card.waitFor();
  await card.locator('[data-action=preview]').tap();
  await page.waitForFunction((text) => document.querySelector('#far').textContent === text, REWRITE);
  await page.waitForTimeout(1200);  // the sheet's move and the smooth scroll
  const height = (await sheet.boundingBox()).height;
  assert.ok(Math.abs(height - 844 * 0.4) < 2, `the sheet went down to half height: ${height}`);
  const far = await page.locator('#far').boundingBox();
  assert.ok(far.y >= 0 && far.y + far.height <= 844 - height, `the preview is above the sheet: ${far.y}..${far.y + far.height}, the sheet from ${844 - height}`);
  const middle = far.y + far.height / 2;
  assert.ok(Math.abs(middle - (844 - height) / 2) < 60, `near the middle of what shows: ${middle}`);
  assert.deepEqual(errors, []);
  await context.close();
});

test('on a phone, a selection fits one annotation: across blocks or past 600 characters it is refused; a preview empties no other node', async () => {
  const { context, page, sheet, launcher } = await phone();
  const fits = () => page.evaluate(() => tinyactorsCopywriter.reviewer.getState().selection?.fits);
  // across two blocks: the lede into the heading after it
  await page.evaluate(() => {
    const range = document.createRange();
    range.setStart(document.querySelector('#lede').firstChild, 2);
    range.setEnd(document.querySelector('h2').firstChild, 5);
    getSelection().removeAllRanges();
    getSelection().addRange(range);
  });
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection?.fits === false);
  assert.equal(await launcher.getAttribute('aria-label'), 'Open copywriter', 'the launcher offers no annotation');
  await launcher.tap();
  const hint = sheet.locator('[data-banner=refused]');
  await hint.waitFor();
  assert.match(await hint.textContent(), /Select one sentence, heading or paragraph\./);
  assert.equal(await sheet.locator('[data-action=annotate]').count(), 0);
  const refused = await page.evaluate(() => tinyactorsCopywriter.reviewer.dispatch({ type: 'annotate' }).then(() => 'annotated', (e) => e.message));
  assert.equal(refused, 'Select one sentence, heading or paragraph.');

  // one paragraph of more than 600 characters
  await page.evaluate(() => getSelection().selectAllChildren(document.querySelector('#long')));
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection?.text.startsWith('The long paragraph'));
  assert.equal(await fits(), false);

  // one heading over two Text nodes (its <em>): the first takes the text, the second loses only its selected part
  await page.evaluate(() => {
    const h1 = document.querySelector('h1');
    const range = document.createRange();
    range.setStart(h1.firstChild, 7);              // "as a "
    range.setEnd(h1.querySelector('em').firstChild, 3); // "lib"
    getSelection().removeAllRanges();
    getSelection().addRange(range);
  });
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection?.text === 'as a lib');
  assert.equal(await fits(), true);
  const shown = await page.evaluate(async () => {
    const { reviewer } = tinyactorsCopywriter;
    const id = await reviewer.dispatch({ type: 'annotate' });
    await reviewer.dispatch({ type: 'edit', id, text: 'in one ' });
    const h1 = document.querySelector('h1');
    const during = [h1.textContent, h1.querySelector('em').textContent, document.querySelector('#lede').textContent];
    await reviewer.dispatch({ type: 'undo', id });
    return [...during, h1.textContent];
  });
  assert.deepEqual(shown, ['Actors in one rary', 'rary', 'A tiny, embeddable actor runtime for building reliable systems. Get started in minutes.', 'Actors as a library']);
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
  await page.waitForTimeout(400);  // the sheet slides in
  const box = await sheet.boundingBox();
  assert.deepEqual([box.x, box.y, box.width, box.height], [880, 0, 400, 800]);
  await page.locator('#tinyactors-copywriter .close').click();
  await page.waitForFunction(() => !document.getElementById('tinyactors-copywriter').hasAttribute('data-open'));
  await context.close();
});

const LINKED = 'Read the hibernation guide to keep idle actors on disk.';

test('on a phone, pick mode: one tap enters it, a tap picks a block (twice: its parent), no link opens, no text is selected; a message annotates it', async () => {
  const { context, page, errors, sheet, launcher } = await phone();
  // one audience: one rewrite at a time, so the sheet does not rise to compare
  await page.evaluate(() => tinyactorsCopywriter.reviewer.dispatch({ type: 'settings.save', audienceIDs: ['site-tech-lead'] }));
  await launcher.tap();
  await sheet.locator('.cw-panel').waitFor();
  await page.waitForTimeout(400);
  const open = (await sheet.boundingBox()).height;
  const pick = sheet.locator('[data-action=pick]');
  await pick.tap();
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().picking);
  assert.equal(await pick.getAttribute('aria-pressed'), 'true');
  await page.waitForTimeout(400);
  assert.ok((await sheet.boundingBox()).height < open, 'the sheet lowers');
  assert.equal(await page.locator('#lede').evaluate((el) => getComputedStyle(el).userSelect), 'none', 'no text selection while picking');

  // a tap on the link picks the link's text, and the page stays
  const url = page.url();
  await page.locator('#guide-link').scrollIntoViewIfNeeded();
  await page.locator('#guide-link').tap();
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().selection?.text === 'hibernation guide');
  assert.equal(page.url(), url, 'the link did not navigate');
  assert.equal(await page.evaluate(() => getSelection().toString()), '', 'no text selection was made');
  const outline = page.locator('[data-copywriter-outline=picked]');
  const near = (a, b) => Math.abs(a.x - b.x) < 2 && Math.abs(a.y - b.y) < 2 && Math.abs(a.width - b.width) < 2 && Math.abs(a.height - b.height) < 2;
  await page.waitForTimeout(200);
  assert.ok(near(await outline.boundingBox(), await page.locator('#guide-link').boundingBox()), 'the outline is over the link');

  // the same element again: its paragraph
  await page.locator('#guide-link').tap();
  await page.waitForFunction((text) => tinyactorsCopywriter.reviewer.getState().selection?.text === text, LINKED);
  await page.waitForTimeout(200);
  assert.ok(near(await outline.boundingBox(), await page.locator('#linked').boundingBox()), 'the outline is over the paragraph');
  // the lowered sheet shows the pick's banner and the composer whole
  const dock = await sheet.locator('.cw-dock').boundingBox();
  assert.ok(dock.y + dock.height <= 844 + 1, `the composer is in view: ${dock.y + dock.height}`);
  const linked = await page.locator('#linked').boundingBox();
  assert.ok(linked.y + linked.height <= (await sheet.boundingBox()).y, 'the paragraph is above the sheet');
  assert.equal(page.url(), url);

  // a message annotates the pick; pick mode ends and the sheet rises again
  const composer = sheet.locator('textarea[aria-label=Message]');
  await composer.fill('Name the guide');
  await composer.press('Enter');
  await sheet.locator('[data-view=detail]').waitFor();
  assert.equal(await sheet.locator('.cw-original').textContent(), LINKED);
  await page.waitForFunction(() => !tinyactorsCopywriter.reviewer.getState().picking);
  await page.waitForTimeout(400);
  assert.ok(Math.abs((await sheet.boundingBox()).height - open) < 2, 'back to its height');
  assert.equal(await outline.count(), 0, 'the outline goes with pick mode');
  assert.notEqual(await page.locator('#lede').evaluate((el) => getComputedStyle(el).userSelect), 'none', 'text selects again');
  assert.deepEqual(errors, []);
  await context.close();
});

test('on a phone, pick mode ends with Escape on the page and with the pick button', async () => {
  const { context, page, sheet, launcher } = await phone();
  await launcher.tap();
  await sheet.locator('.cw-panel').waitFor();
  await sheet.locator('[data-action=pick]').tap();
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().picking);
  await page.evaluate(() => document.activeElement?.blur?.());
  await page.keyboard.press('Escape');
  await page.waitForFunction(() => !tinyactorsCopywriter.reviewer.getState().picking);
  await sheet.locator('[data-action=pick]').tap();
  await page.waitForFunction(() => tinyactorsCopywriter.reviewer.getState().picking);
  await sheet.locator('[data-action=pick]').tap();
  await page.waitForFunction(() => !tinyactorsCopywriter.reviewer.getState().picking);
  await context.close();
});

test('a database of version 1: the page\'s chat is its first conversation, an edited default persona the voice', async () => {
  const context = await browser.newContext(PHONE);
  await context.addInitScript(() => {
    localStorage.setItem('tinyactors-copywriter', 'on');
    sessionStorage.setItem('tinyactors-copywriter-key', 'AIza-test-key');
  });
  const page = await context.newPage();
  // version 1, as the copywriter left it before 2026-11-23, made on the page's origin before the widget loads
  await page.goto(`${origin(server)}/copywriter/`);
  await page.evaluate((url) => new Promise((resolve, reject) => {
    const open = indexedDB.open('copywriter', 1);
    open.onupgradeneeded = () => {
      const db = open.result;
      db.createObjectStore('annotations', { keyPath: ['page', 'id'] }).createIndex('page', 'page');
      db.createObjectStore('chats', { keyPath: 'page' }).put({ page: url, messages: [{ role: 'user', text: 'Old question', at: '2026-11-20T10:00:00.000Z' }, { role: 'model', text: 'Old answer', at: '2026-11-20T10:00:05.000Z' }] });
      db.createObjectStore('settings', { keyPath: 'key' }).put({ key: 'settings', value: { model: 'gemini-4-hydrogen', personaID: 'terse', thinking: 'low' } });
      const personas = db.createObjectStore('personas', { keyPath: 'id' });
      personas.put({ id: 'tinyactors-voice', name: 'tinyactors voice', instructions: 'Edited voice.' });
      personas.put({ id: 'terse', name: 'Terse', instructions: 'Five words.' });
    };
    open.onsuccess = () => { open.result.close(); resolve(); };
    open.onerror = () => reject(open.error);
  }), `${origin(server)}/page/page.html`);
  await page.goto(`${origin(server)}/page/page.html`);
  await page.waitForFunction(() => globalThis.tinyactorsCopywriter?.reviewer.getState().status === 'ready');
  const state = await page.evaluate(() => tinyactorsCopywriter.reviewer.getState());
  assert.equal(state.chat.id, 'c0');
  assert.deepEqual(state.chat.messages.map((m) => m.text), ['Old question', 'Old answer']);
  assert.deepEqual(state.settings.voice, { instructions: 'Edited voice.', custom: true });
  assert.equal(state.settings.thinking, 'low', 'the other settings stay');
  // no audience came from version 1; the page's linked files are added, and are its set
  assert.deepEqual(state.settings.audiences.map((a) => a.source), ['site', 'site', 'site']);
  assert.deepEqual(state.settings.audienceIDs, ['site-agent-platform', 'site-tech-lead', 'site-workflow-tester']);
  await context.close();
});

test('the widget reads the page\'s audiences from its links: the files are fetched, their chips appear with no audience of the copywriter\'s own', async () => {
  const { context, page, errors, sheet, launcher } = await phone();
  const state = await page.evaluate(() => tinyactorsCopywriter.reviewer.getState());
  assert.equal(state.page.kind, 'pitch');
  assert.equal(state.page.mainAudienceID, 'site-agent-platform');
  assert.deepEqual(state.settings.audiences.map((a) => [a.id, a.source, a.url.replace(/^http:\/\/[^/]+/, '')]), [
    ['site-agent-platform', 'site', '/page/audiences/agent-platform.xml'],
    ['site-tech-lead', 'site', '/page/audiences/tech-lead.xml'],
    ['site-workflow-tester', 'site', '/page/audiences/workflow-tester.xml'],
  ], 'parsed with the browser\'s DOMParser');
  assert.equal(state.settings.audiences[1].items[1].id, 'failure-semantics');
  assert.deepEqual(state.settings.audienceIDs, ['site-agent-platform', 'site-tech-lead', 'site-workflow-tester']);
  const purpose = await page.evaluate(() => tinyactorsCopywriter.reviewer.getState().page);
  assert.equal(purpose.audienceSuggestion, null, 'not asked');
  await launcher.tap();
  const chips = sheet.locator('.cw-strip-chip');
  await chips.first().waitFor();
  assert.equal(await chips.count(), 3);
  assert.deepEqual(await chips.locator('.cw-avatar').allTextContents(), ['AP', 'TL', 'WD']);
  assert.match(await chips.nth(1).locator('.cw-site').getAttribute('title'), /\/page\/audiences\/tech-lead\.xml$/);
  if (process.env.CW_SHOTS) await page.screenshot({ path: `${process.env.CW_SHOTS}/widget-site-audiences.png` });
  assert.deepEqual(errors, []);
  await context.close();
});
