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
  const params = new URLSearchParams('bare=1&delay=0.05');
  for (const [key, value] of new URLSearchParams(query)) params.set(key, value);
  await page.goto(`${base}?${params}`);
  await page.locator('#panel .cw-panel').waitFor();
  return { page, panel: page.locator('#panel'), errors };
}

// Polls `fn` (async, in node) until it is truthy.
async function until(fn, ms = 6000) {
  const end = Date.now() + ms;
  for (;;) {
    if (await fn()) return;
    if (Date.now() > end) throw new Error(`not in ${ms} ms: ${fn}`);
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
}

const commands = (page) => page.evaluate(() => reviewer.log.map((c) => c.type));

test('the list shows the annotations, filtered', async () => {
  const { page, panel, errors } = await open('read=0');
  assert.equal(await panel.locator('.cw-row').count(), 3, 'open: suggested, previewing, draft');
  const done = panel.locator('[data-action=done]');
  assert.equal(await done.getAttribute('aria-label'), 'Done: 1 kept, 0 dismissed');
  assert.equal(await done.getAttribute('aria-expanded'), 'false', 'folded while annotations are open');
  await done.click();
  assert.equal(await panel.locator('.cw-row').count(), 4);
  assert.equal(await panel.locator('.cw-heading').textContent(), 'Actors as a library');
  assert.equal(await panel.locator('.cw-heading').getAttribute('title'), 'https://www.tinyactors.dev/');
  for (const name of ['Page chat', 'Settings', 'Export']) {
    assert.equal(await panel.locator(`header button[aria-label="${name}"]`).count(), 1, `${name} is an icon with a name`);
  }
  assert.equal(await panel.locator('main, [role=main]').count(), 0, 'the host page keeps its one main landmark');
  assert.deepEqual(errors, []);
  await page.close();
});

test('the done annotations unfold by themselves when none is open', async () => {
  const { page, panel } = await open('');
  await page.evaluate(() => reviewer.set({ annotations: reviewer.getState().annotations.filter((a) => a.state === 'kept') }));
  await panel.locator('.cw-row[data-state=kept]').waitFor();
  assert.equal(await panel.locator('[data-action=done]').getAttribute('aria-expanded'), 'true');
  await page.close();
});

test('a selection across blocks: the banner says what fits, offers no Annotate, and goes', async () => {
  const { page, panel } = await open('');
  await page.evaluate(() => reviewer.setSelection('Actors as a library. Everything you need', { fits: false }));
  const hint = panel.locator('[data-banner=refused]');
  await hint.waitFor();
  assert.match(await hint.textContent(), /Select one sentence, heading or paragraph\./);
  assert.equal(await panel.locator('[data-action=annotate]').count(), 0);
  assert.equal(await panel.locator('textarea[aria-label=Message]').getAttribute('placeholder'), 'Ask about this page');
  await until(async () => (await hint.evaluate((n) => getComputedStyle(n).visibility)) === 'hidden');
  await page.close();
});

test('Annotate from the keyboard: the new annotation\'s composer has the focus', async () => {
  const { page, panel } = await open('');
  await page.evaluate(() => reviewer.setSelection('Everything you need to build actor systems'));
  const annotate = panel.locator('[data-banner=selection] [data-action=annotate]');
  await annotate.focus();
  await page.keyboard.press('Enter');
  await panel.locator('[data-view=detail]').waitFor();
  const composer = panel.locator('textarea[aria-label=Message]');
  await until(() => composer.evaluate((node) => node === node.getRootNode().activeElement));
  assert.equal(await composer.getAttribute('placeholder'), 'Ask for a change');
  await page.close();
});

test('the detail reads as a conversation: the quote, the note, each reply with its suggestions after it', async () => {
  const { page, panel } = await open('');
  await panel.locator('.cw-row[data-id=a4]').click();
  const detail = panel.locator('[data-view=detail]');
  await detail.waitFor();
  const order = await detail.evaluate((root) => [...root.querySelectorAll('.cw-original, .cw-note, .cw-msg, .cw-card')]
    .map((n) => n.classList.contains('cw-card') ? 'card' : n.classList.contains('cw-msg--user') ? 'user' : n.classList.contains('cw-msg') ? 'model' : n.classList[0]));
  assert.deepEqual(order, ['cw-original', 'cw-note', 'user', 'model', 'card', 'card']);
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('Shorter');
  await composer.press('Enter');
  await until(async () => (await detail.locator('.cw-card').count()) > 2);
  const main = panel.locator('.cw-main');
  await until(() => main.evaluate((m) => m.scrollHeight - m.scrollTop - m.clientHeight < 4));
  const flow = await detail.evaluate((root) => [...root.querySelectorAll('.cw-msg, .cw-card')].map((n) => n.classList.contains('cw-card') ? 'card' : n.classList.contains('cw-msg--user') ? 'user' : 'model').join(' '));
  assert.match(flow, /^user model card card user model( card)+$/, 'the newest reply and its cards at the bottom');
  assert.ok(await main.count());
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
  await panel.locator('[data-thinking]').first().waitFor();
  await detail.locator('.cw-card').first().waitFor();
  assert.ok(await detail.locator('.cw-msg--user').count() >= 1);
  assert.ok(await detail.locator('.cw-msg--model strong').count() >= 1, 'markdown bold');

  assert.equal(await detail.locator('[data-action=keep]').count(), 0, 'Keep appears only on a previewed card');
  await detail.locator('[data-action=preview]').first().click();
  await detail.locator('.cw-card.is-chosen [data-action=keep]').waitFor();
  assert.equal(await detail.locator('.cw-original.is-replaced').count(), 1);
  await detail.locator('[data-action=keep]').click();
  await detail.locator('.cw-card.is-kept').waitFor();

  await page.keyboard.press('Escape');
  await panel.locator('[data-view=list]').waitFor();
  await panel.locator('[data-action=done]').click();
  assert.equal(await panel.locator('.cw-row[data-state=kept]').count(), 2);
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
  assert.equal(await panel.locator('[data-action=add-key]').count(), 1);
  assert.equal(await panel.locator('[data-action=send]').isDisabled(), true);
  await panel.locator('[data-action=add-key]').click();
  await panel.locator('[data-view=settings]').waitFor();
  const key = panel.locator('input.cw-key');
  assert.equal(await key.getAttribute('type'), 'password');
  await key.fill('AIza-test-key');
  await panel.locator('[data-action=show-key]').click();
  assert.equal(await key.getAttribute('type'), 'text');
  assert.equal(await key.inputValue(), 'AIza-test-key', 'the draft survives the rebuild');
  assert.equal(await panel.locator('a[data-action=get-key]').getAttribute('href'), 'https://aistudio.google.com/apikey');
  // a key Google refuses: said in a toast above the composer, not saved
  await key.fill('AIza-bad');
  await panel.locator('[data-action=save-key]').click();
  const refused = panel.locator('.cw-toast--error', { hasText: 'That key was not accepted by Google.' });
  await refused.waitFor();
  assert.equal(await panel.locator('.cw-label .cw-ok').count(), 0, 'not shown as saved');
  const toastBox = await refused.boundingBox();
  for (const label of await panel.locator('.cw-label').all()) {
    const box = await label.boundingBox();
    assert.ok(toastBox.y >= box.y + box.height || toastBox.y + toastBox.height <= box.y, 'the toast covers no label');
  }
  await refused.locator('button').click();
  await key.fill('AIza-test-key');
  await panel.locator('[data-action=save-key]').click();
  await panel.locator('.cw-label .cw-ok').waitFor();
  assert.equal(await panel.locator('[data-action=save-key]').isHidden(), true, 'Save appears only with a key typed');
  await panel.locator('.cw-toast', { hasText: 'Key saved' }).waitFor();
  assert.equal(await key.inputValue(), '');

  await panel.locator('#cw-model').selectOption('gemini-3.8-flash');
  await page.waitForFunction(() => reviewer.getState().settings.model === 'gemini-3.8-flash');
  await panel.locator('[data-action=refresh-models]').click();
  await page.waitForFunction(() => reviewer.getState().settings.models.length === 4);

  const log = await page.evaluate(() => reviewer.log);
  assert.equal(log.findLast((c) => c.type === 'settings.save' && c.apiKey).apiKey, 'AIza-test-key');
  await page.keyboard.press('Escape');
  await panel.locator('[data-view=list]').waitFor();
  assert.equal(await panel.locator('[data-action=add-key]').count(), 0);
  await page.close();
});

test('export: the menu, a download and a refusal', async () => {
  const { page, panel } = await open('');
  await panel.locator('[data-menu=export]').click();
  const menu = panel.locator('.cw-menu[role=menu]');
  await menu.waitFor();
  await menu.locator('[data-scope=all]').click();
  await menu.locator('[data-action=export-download]').click();
  await panel.locator('.cw-toast', { hasText: 'Work packet downloaded' }).waitFor();
  assert.deepEqual((await page.evaluate(() => reviewer.log)).at(-1), { type: 'export', scope: 'all', to: 'download' });

  await page.evaluate(() => reviewer.set({ totals: { kept: 0, open: 1, pages: 1 } }));
  await panel.locator('[data-menu=export]').click();
  await page.keyboard.press('Escape');
  assert.equal(await menu.count(), 0, 'Escape closes the menu');
  await panel.locator('[data-menu=export]').click();
  await panel.locator('[data-action=export-clipboard]').click();
  await panel.locator('.cw-toast--error', { hasText: 'Nothing is kept yet' }).waitFor();
  await page.close();
});

test('page chat: a message, the answer, a link to the annotation it proposed', async () => {
  const { page, panel } = await open('read=0');
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('What should change?');
  await composer.press('Enter');
  await panel.locator('[data-view=chat] .cw-msg--model .cw-msg-body').waitFor();
  assert.equal(await panel.locator('[data-action=chat]').getAttribute('aria-pressed'), 'true');
  assert.equal(await page.evaluate(() => reviewer.getState().annotations.length), 5);
  const link = panel.locator('[data-view=chat] .cw-msg--model [data-action=open-annotation]');
  assert.match(await link.textContent(), /Everything you need to build actor systems.*Spawn, send and supervise actors/);
  await link.click();
  await panel.locator('[data-view=detail] .cw-card.is-chosen').waitFor();
  const log = (await commands(page)).slice(-2);
  assert.deepEqual(log, ['select', 'preview'], 'it opens and previews the proposal');
  await page.keyboard.press('Escape');
  await panel.locator('[data-view=chat]').waitFor();
  await page.keyboard.press('Escape');
  await panel.locator('[data-view=list]').waitFor();
  assert.equal(await panel.locator('.cw-row').count(), 4, 'the proposed rewrite is an open annotation');
  await page.close();
});

test('page chat: a reply that only proposed shows the proposals as its links; an error as the annotations\' do', async () => {
  const { page, panel } = await open('');
  await page.evaluate(() => {
    const [a] = reviewer.getState().annotations;
    reviewer.set({ chat: { busy: false, error: 'Gemini\'s rate limit or quota is reached (429): wait a minute and try again.', messages: [
      { role: 'user', text: 'Weakest line?', at: 1 },
      { role: 'model', text: `“${a.original}” → ${a.suggestions[0].text}`, proposed: true, annotations: [a.id], at: 2 },
    ] } });
  });
  await panel.locator('[data-action=chat]').click();
  const reply = panel.locator('[data-view=chat] .cw-msg--model');
  await reply.locator('[data-action=open-annotation]').waitFor();
  assert.equal(await reply.locator('.cw-msg-body').count(), 0, 'the links say it: no text again');
  const error = panel.locator('[data-view=chat] .cw-error[role=alert]');
  assert.match(await error.textContent(), /429/);
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
  await page.waitForFunction(() => reviewer.getState().recording.state === 'idle');
  await page.waitForTimeout(20);
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

test('voice: a waiting bubble while it transcribes, then the transcript as the user\'s bubble with a mic', async () => {
  const { page, panel, errors } = await open('delay=1');
  await panel.locator('.cw-row[data-id=a4]').click();
  const mic = panel.locator('[data-action=mic]');
  await mic.click(); // a tap starts
  await page.waitForFunction(() => reviewer.getState().recording.state === 'recording');
  await mic.click(); // a tap stops
  await panel.locator('[data-view=detail] [data-transcribing]').waitFor();
  const spoken = panel.locator('[data-view=detail] .cw-msg--user .cw-msg-body.is-spoken').filter({ hasText: 'lead with the number' });
  await spoken.waitFor();
  assert.equal(await spoken.locator('.cw-spoken svg').count(), 1, 'a small mic, no label');
  assert.equal(await panel.locator('[data-view=detail] [data-transcribing]').count(), 0);
  assert.ok(!(await panel.locator('[data-view=detail]').textContent()).includes('(voice message)'));
  assert.deepEqual(errors, []);
  await page.close();
});

test('thinking: a reply shows a quiet "Thought for N s" that unfolds its summary', async () => {
  const { page, panel } = await open('');
  await panel.locator('.cw-row[data-id=a4]').click();
  const thought = panel.locator('[data-view=detail] .cw-thought');
  await thought.waitFor();
  assert.equal((await thought.locator('summary').textContent()).trim(), 'Thought for 9 s');
  assert.equal(await thought.locator('.cw-thought-body').isVisible(), false, 'collapsed');
  await thought.locator('summary').click();
  assert.match(await thought.locator('.cw-thought-body').textContent(), /strongest fact/);
  // the answer is the text, not the thoughts
  assert.ok(!(await panel.locator('[data-view=detail] .cw-msg--model .cw-msg-body').first().textContent()).includes('Finding the strongest'));
  await page.close();
});

test('audiences: the chat\'s chip opens the audience in Settings, where a switch, the editor and delete work', async () => {
  const { page, panel, errors } = await open('read=0');
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('Who is this page for?');
  await composer.press('Enter');
  const chip = panel.locator('[data-view=chat] .cw-audience-chip');
  await chip.waitFor();
  assert.equal(await chip.textContent(), 'Skeptical backend engineer');
  assert.match(await chip.getAttribute('class'), /is-active/);
  await chip.click();
  await panel.locator('[data-view=settings]').waitFor();
  const id = await page.evaluate(() => reviewer.getState().settings.audiences.at(-1).id);
  const row = panel.locator(`.cw-audience[data-audience="${id}"]`);
  assert.equal(await row.locator('.cw-input').inputValue(), 'Skeptical backend engineer', 'opened on it');
  assert.equal(await row.locator('textarea[aria-label="Their objections"]').inputValue(), 'Hype.');
  // the switch: writing for it or not
  await row.locator('.cw-switch').click();
  await page.waitForFunction((x) => !reviewer.getState().settings.audienceIDs.includes(x), id);
  await row.locator('.cw-switch').click();
  await page.waitForFunction((x) => reviewer.getState().settings.audienceIDs.includes(x), id);
  // edit and save
  await row.locator('textarea[aria-label="What they want here"]').fill('Memory per actor and throughput.');
  await row.locator('[data-action=save-audience]').click();
  await page.waitForFunction((x) => reviewer.getState().settings.audiences.find((a) => a.id === x).wants === 'Memory per actor and throughput.', id);
  // a new one
  await panel.locator('[data-action=add-audience]').click();
  await panel.locator('.cw-audience-edit .cw-input').fill('CTO');
  await panel.locator('.cw-audience-edit textarea[aria-label="Who they are"]').fill('Signs the budget.');
  await panel.locator('.cw-audience-edit [data-action=save-audience]').click();
  await page.waitForFunction(() => reviewer.getState().settings.audiences.some((a) => a.name === 'CTO'));
  // delete
  await row.locator('[data-action=edit-audience]').click();
  await row.locator('[data-action=delete-audience]').click();
  await page.waitForFunction((x) => !reviewer.getState().settings.audiences.some((a) => a.id === x), id);
  assert.deepEqual(await page.evaluate(() => reviewer.getState().settings.audiences.map((a) => a.name)), ['Student', 'CTO']);
  assert.deepEqual(errors, []);
  await page.close();
});

test('settings: the voice is edited, saved and set back to the default', async () => {
  const { page, panel } = await open('');
  await panel.locator('[data-action=settings]').click();
  const voice = panel.locator('#cw-voice');
  assert.match(await voice.inputValue(), /tinyactors\.dev/);
  assert.equal(await panel.locator('[data-action=reset-voice]').count(), 0, 'the default: nothing to reset');
  await voice.fill('Short words only.');
  await panel.locator('[data-action=save-voice]').click();
  await page.waitForFunction(() => reviewer.getState().settings.voice.custom);
  await panel.locator('[data-action=reset-voice]').click();
  await page.waitForFunction(() => !reviewer.getState().settings.voice.custom);
  assert.match(await voice.inputValue(), /tinyactors\.dev/, 'the editor follows');
  await page.close();
});

test('conversations: a new one from the chat\'s header; the earlier ones are listed in an empty one and reopen', async () => {
  const { page, panel } = await open('');
  await panel.locator('[data-action=chat]').click();
  const earlier = panel.locator('.cw-conversations [data-action=open-chat]');
  assert.equal(await earlier.count(), 1, 'an earlier conversation');
  assert.equal(await panel.locator('[data-action=new-chat]').isVisible(), false, 'an empty one needs no new one');
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('What should change?');
  await composer.press('Enter');
  await panel.locator('[data-view=chat] .cw-msg--model .cw-msg-body').waitFor();
  await panel.locator('[data-action=new-chat]').click();
  await page.waitForFunction(() => reviewer.getState().chat.messages.length === 0);
  await earlier.nth(1).waitFor();
  assert.equal(await earlier.count(), 2);
  assert.match(await earlier.first().textContent(), /What should change\?now/);
  await earlier.filter({ hasText: 'Which claims need a number?' }).click();
  await panel.locator('[data-view=chat] .cw-msg--model', { hasText: 'Blazing-fast' }).waitFor();
  assert.deepEqual((await commands(page)).filter((c) => c.startsWith('chat.')), ['chat.new', 'chat.open']);
  await page.close();
});

test('settings: the answers\' length and Google\'s tools, one compact row each', async () => {
  const { page, panel } = await open('');
  await panel.locator('[data-action=settings]').click();
  const length = panel.locator('[role=group][aria-labelledby=cw-length]');
  assert.equal(await length.locator('[aria-pressed=true]').textContent(), 'Short');
  await length.locator('[data-length=long]').click();
  await page.waitForFunction(() => reviewer.getState().settings.length === 'long');
  const tools = panel.locator('[role=group][aria-label=Tools]');
  assert.deepEqual(await tools.locator('.cw-switch').evaluateAll((all) => all.map((x) => [x.dataset.tool, x.checked])), [['search', false], ['url', true], ['code', false]]);
  await tools.locator('[data-tool=search]').click();
  await page.waitForFunction(() => reviewer.getState().settings.tools.search);
  const saved = (await page.evaluate(() => reviewer.log)).filter((c) => c.type === 'settings.save').at(-1);
  assert.deepEqual(saved, { type: 'settings.save', tools: { search: true } });
  await page.close();
});

test('a grounded answer shows its sources as small numbered links', async () => {
  const { page, panel } = await open('');
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('How do other actor runtimes describe themselves? Use search.');
  await composer.press('Enter');
  const sources = panel.locator('[data-view=chat] .cw-sources a');
  await sources.first().waitFor();
  assert.deepEqual(await sources.evaluateAll((all) => all.map((a) => [a.textContent, a.href, a.target])), [
    ['1akka.io', 'https://akka.io/', '_blank'], ['2learn.microsoft.com', 'https://learn.microsoft.com/dotnet/orleans/', '_blank'],
  ]);
  await page.close();
});

test('settings: the thinking level is a segmented control', async () => {
  const { page, panel } = await open('');
  await panel.locator('[data-action=settings]').click();
  const group = panel.locator('[role=group][aria-labelledby=cw-thinking]');
  assert.equal(await group.locator('[aria-pressed=true]').textContent(), 'High');
  await group.locator('[data-thinking=xhigh]').click();
  await page.waitForFunction(() => reviewer.getState().settings.thinking === 'xhigh');
  await group.locator('[data-thinking=xhigh][aria-pressed=true]').waitFor();
  const saved = (await page.evaluate(() => reviewer.log)).filter((c) => c.type === 'settings.save').at(-1);
  assert.deepEqual(saved, { type: 'settings.save', thinking: 'xhigh' });
  await page.close();
});

test('sheet: tap targets of 44px, light and dark', async () => {
  for (const colorScheme of ['light', 'dark']) {
    const { page, panel, errors } = await open('layout=sheet', { viewport: { width: 390, height: 844 }, colorScheme, hasTouch: true });
    await page.evaluate(() => reviewer.setSelection('Everything you need to build actor systems'));
    await panel.locator('[data-banner=selection]').waitFor();
    const small = await panel.locator('.cw-panel').evaluate((root) => [...root.querySelectorAll('button')]
      .filter((b) => b.offsetParent && !b.closest('.cw-toast'))
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

test('read as an audience: the strip\'s chip shows how much the page answers; its menu reads again; the chip spins, then counts', async () => {
  const { page, panel, errors } = await open('');
  const chip = panel.locator('.cw-strip-chip[data-audience=lead]');
  assert.match(await chip.getAttribute('aria-label'), /^Tech lead who approves dependencies · 2 of 6 answered/);
  assert.equal((await chip.textContent()).trim(), 'TL2/6', 'initials and a count, no name');
  await chip.click();
  const menu = panel.locator('.cw-menu[aria-label="Tech lead who approves dependencies"]');
  await menu.waitFor();
  assert.deepEqual(await menu.locator('.cw-menu-item').allTextContents(), ['Read again', 'Checklist', 'Talk to this reader', 'Edit audience']);
  await menu.locator('[data-action=read-as]').click();
  await until(async () => (await chip.getAttribute('data-reading')) === 'reading');
  assert.equal(await chip.locator('.cw-spin').count(), 1);
  await until(async () => (await chip.getAttribute('data-reading')) === 'done');
  assert.equal((await chip.textContent()).trim(), 'TL0/2');
  const read = await page.evaluate(() => reviewer.log.find((c) => c.type === 'read'));
  assert.deepEqual(read, { type: 'read', audienceIDs: ['lead'] });
  assert.deepEqual(errors, []);
  await page.close();
});

test('the checklist: one line per item with its status; an answer shows on the page, a gap becomes an open item', async () => {
  const { page, panel, errors } = await open('');
  await panel.locator('.cw-strip-chip[data-audience=lead]').click();
  await panel.locator('[data-action=open-reading]').click();
  const view = panel.locator('[data-view=reading]');
  await view.waitFor();
  assert.equal(await panel.locator('.cw-heading').textContent(), 'Tech lead who approves dependencies');
  assert.deepEqual(await view.locator('.cw-label').allTextContents(), ['Wants', 'Came to', 'Objections', 'Findings2']);
  assert.deepEqual(await view.locator('.cw-check-item').evaluateAll((items) => items.map((x) => x.dataset.status)),
    ['missing', 'answered', 'partial', 'answered', 'missing', 'partial']);
  assert.equal(await view.locator('.cw-check-glyph[aria-label="Partly answered"]').count(), 2);
  // an answered item: its text on the page, and why
  await view.locator('.cw-check-item[data-index="1"] .cw-check-line').click();
  assert.equal(await view.locator('.cw-check-item[data-index="1"]').getAttribute('class'), 'cw-check-item is-open');
  assert.match(await view.locator('.cw-check-item[data-index="1"] .cw-why').textContent(), /MIT/);
  assert.deepEqual(await page.evaluate(() => reviewer.log.at(-1)), { type: 'reading.show', audienceID: 'lead', index: 1 });
  assert.equal(await view.locator('.cw-check-item[data-status=answered] [data-action=add-gap]').count(), 0, 'nothing to add to an answer');
  // a gap: a quiet + makes it an open item, which the line then links to
  await view.locator('.cw-check-item[data-index="4"] [data-action=add-gap]').click();
  await view.locator('.cw-check-item[data-index="4"] [data-action=open-gap]').waitFor();
  assert.deepEqual(await page.evaluate(() => reviewer.log.at(-1)), { type: 'reading.open', audienceID: 'lead', index: 4 });
  assert.equal(await view.locator('.cw-row').count(), 2, 'the reading\'s findings under the checklist');
  // back: the list, where the gap is an open item to add
  await panel.locator('[data-action=back]').click();
  await panel.locator('.cw-row[data-kind=missing]').first().waitFor();
  assert.equal(await panel.locator('.cw-row.is-page').count(), 2);
  assert.deepEqual(errors, []);
  await page.close();
});

test('findings in the list: the kind as an icon with the audience\'s initials, no words; a page-level one is never previewed', async () => {
  const { page, panel, errors } = await open('');
  const doubt = panel.locator('.cw-row[data-kind=doubt]');
  assert.equal((await doubt.locator('.cw-initials').textContent()), 'TL');
  assert.match(await doubt.getAttribute('title'), /^Claim without proof · Tech lead who approves dependencies · Suggestions ready/);
  assert.equal(await doubt.locator('.cw-row-icon').evaluate((n) => [...n.children].filter((c) => !c.classList.contains('cw-sr')).map((c) => c.textContent).join('').trim()), 'TL', 'the kind is an icon, not a word');
  await panel.locator('.cw-row[data-kind=missing]').click();
  const detail = panel.locator('[data-view=detail]');
  await detail.waitFor();
  assert.equal(await detail.locator('.cw-original.is-page .cw-add').count(), 1);
  assert.match(await detail.locator('.cw-finding').textContent(), /I cannot tell when to say no\./);
  await panel.locator('[data-menu=more]').click();
  assert.equal(await panel.locator('.cw-menu [data-action=select]').count(), 0, 'no "Show on page" for what the page lacks');
  await page.keyboard.press('Escape');
  // the finding's audience opens its checklist
  await detail.locator('.cw-finding-who').click();
  await panel.locator('[data-view=reading]').waitFor();
  // a finding with a rewrite previews it as any suggestion
  await panel.locator('[data-view=reading] .cw-row[data-kind=doubt]').click();
  await panel.locator('[data-view=detail] .cw-card-pick').click();
  await until(async () => (await panel.locator('.cw-card.is-chosen').count()) === 1);
  assert.deepEqual(errors, []);
  await page.close();
});

test('read as: from an audience\'s row in Settings, and from the page chat, whose chip opens the checklist', async () => {
  const { page, panel, errors } = await open('');
  await panel.locator('header [data-action=settings]').click();
  await panel.locator('[data-audience=student] [data-action=read-as]').click();
  await until(async () => (await page.evaluate(() => reviewer.log.some((c) => c.type === 'read' && c.audienceIDs[0] === 'student'))));
  await panel.locator('[data-view=list] .cw-strip-chip[data-audience=student]').waitFor();
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('How would the tech lead read this page?');
  await composer.press('Enter');
  const chip = panel.locator('[data-view=chat] [data-action=open-reading]');
  await chip.waitFor();
  await chip.click();
  await panel.locator('[data-view=reading][data-audience=lead]').waitFor();
  assert.deepEqual(errors, []);
  await page.close();
});

test('talk to an audience: from the chip\'s menu; the header shows who, the reply is the reader\'s, a correction shows what changed', async () => {
  const { page, panel, errors } = await open('');
  await panel.locator('.cw-strip-chip[data-audience=lead]').click();
  await panel.locator('.cw-menu [data-action=talk]').click();
  const view = panel.locator('[data-view=chat]');
  await view.waitFor();
  const heading = panel.locator('.cw-heading.is-reader[data-audience=lead]');
  await heading.waitFor();
  assert.equal(await heading.locator('.cw-avatar').textContent(), 'TL');
  assert.equal(await heading.locator('.cw-heading-name').textContent(), 'Tech lead who approves dependencies');
  assert.deepEqual(await page.evaluate(() => reviewer.log.at(-1)), { type: 'talk', audienceID: 'lead' });
  assert.equal(await panel.locator('textarea[aria-label=Message]').getAttribute('placeholder'), 'Ask this reader');
  // what to ask a reader first
  await view.locator('.cw-chip', { hasText: 'What would make you close this tab?' }).click();
  const reply = view.locator('.cw-msg--reader');
  await reply.waitFor();
  assert.equal(await reply.locator('.cw-msg-avatar').textContent(), 'TL');
  assert.match(await reply.locator('.cw-msg-body').textContent(), /The page doesn't tell me/);
  assert.equal(await view.locator('.cw-msg--model').count(), 0, 'no assistant style');
  // a correction: the audience is updated, and the chip unfolds what changed
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('You\'d actually know Erlang well.');
  await composer.press('Enter');
  const update = view.locator('.cw-update');
  await update.waitFor();
  assert.equal((await update.locator('summary').textContent()).trim(), 'Audience updated');
  await update.locator('summary').click();
  assert.equal(await update.locator('.cw-diff dt').textContent(), 'Knows');
  assert.match(await update.locator('.cw-diff ins').textContent(), /Erlang and OTP/);
  assert.equal(await update.locator('.cw-diff del').count(), 0, 'the mock lead knew nothing before');
  assert.match(await page.evaluate(() => reviewer.getState().settings.audiences.find((a) => a.id === 'lead').knows), /Erlang/);
  // the new-conversation button goes back to the editor
  await panel.locator('[data-action=new-chat]').click();
  await until(async () => (await panel.locator('.cw-heading.is-reader').count()) === 0);
  assert.equal(await page.evaluate(() => reviewer.getState().chat.audienceID), null);
  assert.equal(await panel.locator('.cw-conversation .cw-avatar').first().textContent(), 'TL', 'the reader\'s conversation is listed with its avatar');
  assert.deepEqual(errors, []);
  await page.close();
});

test('talk to an audience: from the checklist, and from the editor (talk_to), whose chip opens the conversation again', async () => {
  const { page, panel, errors } = await open('');
  await panel.locator('.cw-strip-chip[data-audience=lead]').click();
  await panel.locator('.cw-menu [data-action=open-reading]').click();
  await panel.locator('[data-view=reading] [data-action=talk]').click();
  await panel.locator('.cw-heading.is-reader[data-audience=lead]').waitFor();
  // the editor: a new conversation, then "let me talk to"
  await panel.locator('[data-action=new-chat]').click();
  await until(async () => (await panel.locator('.cw-heading.is-reader').count()) === 0);
  const composer = panel.locator('textarea[aria-label=Message]');
  await composer.fill('Let me talk to the tech lead.');
  await composer.press('Enter');
  await panel.locator('.cw-heading.is-reader[data-audience=lead]').waitFor();
  // back in the editor's conversation, its chip opens the reader's again
  const editor = await page.evaluate(() => reviewer.getState().chat.conversations.find((c) => !c.audienceID && /talk to/.test(c.title)).id);
  await panel.locator('[data-action=new-chat]').click();
  await panel.locator(`[data-action=open-chat][data-chat="${editor}"]`).click();
  const chip = panel.locator('[data-view=chat] [data-action=open-talk]');
  await chip.waitFor();
  assert.equal(await chip.locator('.cw-avatar').textContent(), 'TL');
  await chip.click();
  await panel.locator('.cw-heading.is-reader[data-audience=lead]').waitFor();
  assert.deepEqual(errors, []);
  await page.close();
});

test('a gap the reader notes is an open item, linked from its message', async () => {
  const { page, panel, errors } = await open('');
  await page.evaluate(() => reviewer.dispatch({ type: 'talk', audienceID: 'lead' }));
  await panel.locator('[data-action=chat]').click();
  await panel.locator('[data-view=chat] .cw-chip', { hasText: 'What is missing for you?' }).click();
  const link = panel.locator('[data-view=chat] .cw-msg--reader [data-action=open-annotation]');
  await link.waitFor();
  assert.match(await link.textContent(), /Name who maintains it/);
  await link.click();
  await panel.locator('[data-view=detail] .cw-finding').waitFor();
  assert.deepEqual(errors, []);
  await page.close();
});
