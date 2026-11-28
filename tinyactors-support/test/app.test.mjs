// The issue list app in Chromium, under a fake host: the host answers
// ui/initialize with a light or a dark theme, sends the tool's input and
// result as a host does after calling known_issues, grants a fullscreen
// request, and passes the app's tools/call requests to this process's
// service (the stand-ins), as the signed-in user. The app is the file the
// server serves. The issues cover every state, in two repositories and
// several components.
//
// SUPPORT_SCREENSHOTS=<dir> writes the list grouped, the peek open and the
// filter menu open, in both themes, at twice the pixels.

import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { stack } from './stack.mjs';
import { APP } from '../lib/app/index.mjs';
import { toolResult } from '../lib/http.mjs';

let chromium = null;
try { ({ chromium } = createRequire(import.meta.url)('playwright')); } catch { /* reported by the skip below */ }

const napi = 'tinyactors-dev/tinyactors-napi', mono = 'tinyactors-dev/tinyactors';
const fresh = (repo, component, title, minutes = 12) => ({ repo, component, kind: 'tooling', minutes, title, body: title });

// The host page: an iframe of the app and the host's side of the protocol.
const HOST = `<!doctype html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"></head><body style="margin:0">
<iframe id="app" style="border:0;width:100%;height:100vh"></iframe>
<script>
  const frame = document.getElementById('app');
  window.calls = []; window.requests = [];
  window.addEventListener('message', async (event) => {
    if (event.source !== frame.contentWindow) return;
    const m = event.data, reply = (result) => frame.contentWindow.postMessage({ jsonrpc: '2.0', id: m.id, result }, '*');
    if (m.method) window.requests.push(m.method);
    if (m.method === 'ui/initialize') return reply({ protocolVersion: '2026-01-26', hostInfo: { name: 'fake-host', version: '1' },
      hostCapabilities: { serverTools: {} }, hostContext: { theme: window.theme, displayMode: 'inline', availableDisplayModes: ['inline', 'fullscreen'],
        ...window.extraContext } });
    if (m.method === 'ui/notifications/initialized') {
      frame.contentWindow.postMessage({ jsonrpc: '2.0', method: 'ui/notifications/tool-input', params: { arguments: window.toolArguments } }, '*');
      const result = await window.callTool(window.toolName, window.toolArguments);
      return frame.contentWindow.postMessage({ jsonrpc: '2.0', method: 'ui/notifications/tool-result', params: result }, '*');
    }
    if (m.method === 'ui/request-display-mode') {
      reply({ mode: m.params.mode });
      return frame.contentWindow.postMessage({ jsonrpc: '2.0', method: 'ui/notifications/host-context-changed', params: { displayMode: m.params.mode } }, '*');
    }
    if (m.method === 'ui/notifications/size-changed') { window.size = m.params; return; }
    if (m.method === 'tools/call') { window.calls.push(m.params); return reply(await window.callTool(m.params.name, m.params.arguments)); }
    if (m.id !== undefined) reply({});
  });
</script></body></html>`;

describe('the issue list app', { skip: chromium ? false : 'Playwright is not installed here' }, () => {
  let s, bob, ann, browser;
  const id = {};   // the issues, by what they are
  before(async () => {
    s = await stack();
    bob = (await s.signIn('code-bob')).access_token;   // maintains tinyactors and tinyactors-napi
    ann = (await s.signIn('code-ann')).access_token;
    const file = async (key, ...args) => { id[key] = (await s.tool(ann, 'report_issue', fresh(...args))).id; };
    await file('fresh', napi, 'examples', 'mise run examples fails before mise run build', 30);
    await file('triaged', napi, 'src', 'Host.restore loses the clock offset', 20);
    await file('fixed', napi, 'docs', 'the README links to a guide that is missing', 5);
    await file('fixed2', napi, 'lib', 'compileDir skips a chart in a hidden directory', 8);
    await file('closed', napi, 'test', 'a flaky test of the scale gate', 15);
    await file('declined', napi, 'tools', 'the measure task needs bun installed', 3);
    await file('regressed', napi, 'examples', 'the chat example leaks a socket per reconnect', 25);
    await file('mono', mono, 'docs', 'the glossary does not define segment', 10);
    await file('mono2', mono, 'website', 'the capacity guide shows old numbers', 6);
    await s.tool(ann, 'attach_workaround', { id: id.fresh, text: 'run `mise run build` in tinyactors-napi first' });
    await s.tool(bob, 'triage', { id: id.triaged, kind: 'api' });
    await s.tool(bob, 'wontfix', { id: id.declined, reason: 'bun is a pinned tool' });
    const fixes = (...keys) => keys.map((k) => `Fixes-issue: ${id[k]}`).join('\n');
    s.github.commit(1001, 'm2', `fix(napi): the docs, the lib, the scale gate, the chat\n\n${fixes('fixed', 'fixed2', 'closed', 'regressed')}`);
    s.github.commit(1001, 'm3', 'refactor(napi): the chat example');
    await s.advance(600_000);
    await s.tool(bob, 'close', { id: id.closed });
    await s.tool(ann, 'report_issue', { ...fresh(napi, 'examples', 'the chat example leaks a socket per reconnect'), commit: 'm3' });
    browser = await chromium.launch();
  });
  after(async () => { await browser?.close(); await s?.support.close(); });

  // A page with the app under the fake host, for `token`'s user, after `tool` was called with `args`.
  // `phone`: a touch device (isMobile, hasTouch); `context`: more of the host's context (deviceCapabilities, safeAreaInsets)
  async function open({ theme = 'light', token = bob, tool = 'known_issues', args = {}, width = 1180, height = 860, scale = 1, phone = false, context = {} } = {}) {
    const page = phone
      ? await (await browser.newContext({ viewport: { width, height }, deviceScaleFactor: scale, colorScheme: theme, isMobile: true, hasTouch: true })).newPage()
      : await browser.newPage({ viewport: { width, height }, deviceScaleFactor: scale, colorScheme: theme });
    await page.exposeFunction('callTool', async (name, input) => {
      const caller = token ? await s.gateway.caller(token) : null;
      const result = await toolResult(s.gateway, caller, name, input);
      await s.idle();
      return result;
    });
    await page.setContent(HOST);
    await page.evaluate(([t, name, input, extra]) => { window.theme = t; window.toolName = name; window.toolArguments = input; window.extraContext = extra; },
      [theme, tool, args, context]);
    await page.evaluate((html) => { document.getElementById('app').srcdoc = html; }, APP.html);
    const app = page.frameLocator('#app');
    await app.locator('.row').first().waitFor();
    return { page, app, frame: page.frames()[1] };
  }
  const shoot = async (page, name) => {
    if (process.env.SUPPORT_SCREENSHOTS) await page.screenshot({ path: join(process.env.SUPPORT_SCREENSHOTS, `${name}.png`) });
  };
  const row = (app, key) => app.locator(`.row[data-id="${id[key]}"]`);
  const shown = async (app) => app.locator('.row').evaluateAll((rows) => rows.map((r) => r.dataset.id));
  const toast = (app) => app.locator('.toast').last();

  it('groups the open issues by state, with each state\'s icon and the short id, in the host\'s theme', async () => {
    for (const theme of ['light', 'dark']) {
      const { page, app } = await open({ theme, scale: 2 });
      assert.deepEqual(await app.locator('.group-head').allTextContents(), ['▾Regressed1', '▾New3', '▾Triaged1']);
      assert.equal(await app.locator('.row').count(), 5, 'the open issues: new, triaged, regressed');
      assert.equal(await row(app, 'fresh').locator('.id').textContent(), id.fresh.replace(/^tinyactors:/, ''));
      assert.equal(await row(app, 'regressed').locator('.status').getAttribute('data-state'), 'regressed');
      assert.equal(await app.locator('.chip').first().textContent(), 'State is open×');
      assert.equal(await app.locator('html').getAttribute('data-theme'), theme);
      // the header's buttons are buttons, not the design system's uppercase labels (a class name once collided)
      assert.equal(await app.locator('#report').evaluate((b) => getComputedStyle(b.querySelector('.btn-label')).textTransform), 'none');
      const background = await app.locator('body').evaluate((b) => getComputedStyle(b).backgroundColor);
      assert.equal(background, theme === 'dark' ? 'rgb(24, 18, 13)' : 'rgb(244, 238, 226)', 'umber-950 or vellum-100');
      const colours = await app.locator('.group-head .status').evaluateAll((icons) => icons.map((i) => getComputedStyle(i).color));
      assert.equal(new Set(colours).size, 3, 'a colour per state');
      assert.ok(await page.evaluate(() => window.size?.height > 0), 'the app told the host its size');
      await shoot(page, `list-grouped-${theme}`);
      await row(app, 'fresh').click();
      await app.locator('.peek .ident', { hasText: id.fresh }).waitFor();
      await app.getByText('run `mise run build` in tinyactors-napi first').waitFor();
      await shoot(page, `peek-open-${theme}`);
      await page.keyboard.press('Escape');
      await app.locator('.pane--list').click({ position: { x: 5, y: 5 } });
      await app.getByRole('button', { name: 'Filter', exact: true }).click();
      await app.locator('.menu--filter').waitFor();
      await shoot(page, `filter-menu-${theme}`);
      await page.close();
    }
  });

  it('filters with the menu and its chips, and searches', async () => {
    const { page, app } = await open();
    await app.locator('.chip-x').first().click();   // State is open: removed
    assert.equal(await app.locator('.row').count(), 9, 'every state');
    await page.keyboard.press('f');
    await app.locator('.menu--filter').waitFor();
    // the components of the repositories shown: the two with issues, not scxmljs's (no issue there)
    const components = () => app.locator('.menu--filter fieldset', { hasText: 'Component' }).locator('.option').allTextContents();
    const listed = (await components()).map((t) => t.trim());
    assert.ok(listed.includes('tinyactors-core') && listed.includes('examples'), listed.join(' '));
    assert.ok(!listed.includes('conformance') && !listed.includes('site'), `scxmljs's components are absent: ${listed.join(' ')}`);
    await app.locator('.menu--filter fieldset', { hasText: 'Repository' }).locator('.option', { hasText: 'tinyactors-dev/scxmljs' }).locator('input').check();
    const scxmljs = (await components()).map((t) => t.trim());
    assert.deepEqual(scxmljs, ['conformance', 'examples', 'packages', 'site'], 'a repository picked brings its components, and only them');
    await app.locator('.menu--filter fieldset', { hasText: 'Repository' }).locator('.option', { hasText: 'tinyactors-dev/scxmljs' }).locator('input').uncheck();
    await app.locator('.menu--filter fieldset', { hasText: 'Component' }).locator('.option', { hasText: /^docs$/ }).locator('input').check();
    await page.keyboard.press('Escape');
    assert.deepEqual((await shown(app)).sort(), [id.fixed, id.mono].sort());
    assert.equal(await app.locator('.chip').first().textContent(), 'Component is docs×');
    await app.locator('.menu--filter').waitFor({ state: 'detached' });
    await app.locator('.chip-x').first().click();
    await page.keyboard.press('/');
    await page.keyboard.type('clock offset');
    assert.deepEqual(await shown(app), [id.triaged]);
    await page.keyboard.press('Escape');
    assert.equal(await app.locator('.row').count(), 9, 'Esc clears the search');
    await page.keyboard.press('f');
    await app.locator('.menu--filter .option', { hasText: 'a workaround' }).locator('input').check();
    assert.deepEqual(await shown(app), [id.fresh]);
    await page.close();
  });

  it('groups and orders as the display options say', async () => {
    const { page, app } = await open();
    await app.locator('.chip-x').first().click();
    await app.getByRole('button', { name: 'Display' }).click();
    await app.locator('.menu--display label', { hasText: 'Component' }).click();
    const heads = (await app.locator('.group-head').allTextContents()).map((t) => t.replace(/^▾|\d+$/g, ''));
    assert.deepEqual(heads, ['docs', 'examples', 'lib', 'src', 'test', 'tools', 'website']);
    await app.locator('.menu--display label', { hasText: 'No grouping' }).click();
    await app.locator('.menu--display label', { hasText: 'Title' }).click();
    const titles = await app.locator('.row .title').allTextContents();
    assert.deepEqual(titles, [...titles].sort((a, b) => a.localeCompare(b)));
    await app.locator('.menu--display label', { hasText: 'Cost' }).click();
    assert.equal((await shown(app))[0], id.regressed, 'the costliest: two reports of 25 minutes');
    await app.locator('.menu--display label', { hasText: 'Recently updated' }).click();
    assert.equal((await shown(app))[0], id.regressed, 'its regression was the last change');
    await page.close();
  });

  it('moves with the keyboard, opens and closes the peek, shows the shortcuts', async () => {
    const { page, app } = await open();
    const order = await shown(app);
    await app.locator('.mark').click();   // the iframe focused, as a person clicking into it
    await page.keyboard.press('j');
    assert.equal(await app.locator('.row:focus').getAttribute('data-id'), order[0]);
    await page.keyboard.press('j');
    await page.keyboard.press('ArrowDown');
    assert.equal(await app.locator('.row:focus').getAttribute('data-id'), order[2]);
    await page.keyboard.press('k');
    await page.keyboard.press('Enter');
    await app.locator('.peek .ident', { hasText: order[1] }).waitFor();
    await page.keyboard.press('j');   // the peek follows
    await app.locator('.peek .ident', { hasText: order[2] }).waitFor();
    await page.keyboard.press('Escape');
    assert.equal(await app.locator('.peek').count(), 0);
    await page.keyboard.press('?');
    await app.locator('.menu--help').waitFor();
    await page.keyboard.press('Escape');
    await app.getByRole('button', { name: 'Expand' }).click();
    await app.locator('html[data-mode="fullscreen"]').waitFor();
    assert.ok((await page.evaluate(() => window.requests)).includes('ui/request-display-mode'), 'asked the host, on the click');
    await page.close();
  });

  it('triages inline in the peek: kind and title', async () => {
    const { app } = await open();
    await row(app, 'mono').click();
    await app.getByLabel('Kind').selectOption('docs');
    await app.getByLabel('Title').fill('the glossary does not define segment or system');
    await app.getByRole('button', { name: 'Triage', exact: true }).click();
    await app.locator('.peek-title', { hasText: 'the glossary does not define segment or system' }).waitFor();
    await app.locator('.peek .state-label[data-state="triaged"]').waitFor();
    assert.match(await toast(app).textContent(), new RegExp(`^Triaged ${id.mono}`));
    assert.equal(await row(app, 'mono').locator('.status').getAttribute('data-state'), 'triaged');
  });

  it('closes in bulk, and shows the one the server refused', async () => {
    const { page, app } = await open();
    await app.locator('.chip-x').first().click();
    await row(app, 'fixed').click({ modifiers: ['Shift'] });   // shift-click with no anchor: selects it
    await row(app, 'fixed2').locator('.pick').click();
    await app.locator('.bulk-count', { hasText: '2 selected' }).waitFor();
    await s.tool(bob, 'close', { id: id.fixed2 });   // closed meanwhile, elsewhere
    await app.getByRole('button', { name: 'Close (2)' }).click();
    await app.locator('.toast[data-tone="error"]').waitFor();
    const text = await toast(app).textContent();
    assert.match(text, /^Closed 1 of 2\./);
    assert.match(text, new RegExp(`${id.fixed2.replace(/^tinyactors:/, '')}: Refused`));
    assert.equal(await row(app, 'fixed').locator('.status').getAttribute('data-state'), 'closed');
    assert.equal(await app.locator('.bulk').count(), 0, 'the selection is cleared');
    await page.close();
  });

  it('undoes an optimistic change the server refuses', async () => {
    const { page, app } = await open();
    await s.tool(bob, 'triage', { id: id.mono2 });   // triaged elsewhere: the app still shows it new
    await row(app, 'mono2').hover();
    await row(app, 'mono2').getByRole('button', { name: /^Triage/ }).click();
    await app.locator('.toast[data-tone="error"]').waitFor();
    assert.match(await toast(app).textContent(), /only a new issue is triaged/);
    assert.equal(await row(app, 'mono2').locator('.status').getAttribute('data-state'), 'new', 'rolled back');
    await page.close();
  });

  it('opens the issue get_issue was called for, and deletes after a confirmation', async () => {
    const { page, app } = await open({ tool: 'get_issue', args: { id: id.declined } });
    await app.locator('.peek .ident', { hasText: id.declined }).waitFor();
    await app.locator('.peek .state-label[data-state="wontfix"]').waitFor();
    await app.getByRole('button', { name: 'Delete…' }).click();
    await app.getByLabel('Reason, for the ledger (optional)').fill('a test report');
    await app.getByRole('button', { name: 'Delete for good' }).click();
    await app.locator('.toast', { hasText: `Deleted ${id.declined}` }).waitFor();
    // the videos (2026-11-28): toasts piled up over the controls; one message now, in the flow under the header
    assert.equal(await app.locator('.toast').count(), 1, 'one message at a time');
    assert.equal(await app.locator('.toasts').evaluate((t) => getComputedStyle(t).position), 'static', 'in the flow, over nothing');
    assert.equal(await app.locator(`.row[data-id="${id.declined}"]`).count(), 0);
    assert.match((await s.tool(null, 'get_issue', { id: id.declined })).reason, /was deleted/);
    const names = await page.evaluate(() => window.calls.map((c) => c.name));
    for (const name of ['list_projects', 'whoami', 'known_issues', 'get_issue', 'delete_issue'])
      assert.ok(names.includes(name), `the app called ${name}`);
    await page.close();
  });

  it('offers a reporter no maintainer\'s actions, and anyone signed in the report form', async () => {
    const { page, app } = await open({ token: ann, width: 700 });
    await row(app, 'fresh').click();
    await app.getByRole('button', { name: 'Attach workaround' }).waitFor();
    assert.equal(await app.getByRole('button', { name: 'Delete…' }).count(), 0);
    assert.equal(await app.locator('.pane--list').isHidden(), true, 'narrow: the peek is over the list');
    await page.keyboard.press('Escape');
    await page.keyboard.press('c');
    await app.getByLabel('Repository').selectOption(napi);
    await app.getByLabel('Component').selectOption('lib');
    await app.getByLabel('What went wrong, in a line').fill('lib exports nothing for bun');
    await app.getByRole('button', { name: 'Report', exact: true }).click();
    await app.locator('.peek-title', { hasText: 'lib exports nothing for bun' }).waitFor();
    assert.match(await app.locator('.toast').first().textContent(), /^Filed tinyactors:tinyactors-napi:\d+/);
    await page.close();
  });

  // --- a phone ------------------------------------------------------------------------
  // What a phone in claude.ai showed (2026-11-28): the peek's header (identifier, copy, previous, next,
  // close) on one unbreakable row made the view 339 px wide in a 320 px frame, so a swipe scrolled the whole
  // view sideways and cut its edges. Below 600 px the view has a phone layout; on touch every control is 44 px.
  const overflow = (frame) => frame.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  // every visible control smaller than 44 × 44 px (a menu's checkbox counts by its whole option row)
  const small = (frame) => frame.evaluate(() => [...document.querySelectorAll(
    'button, a[href], select, input:not([type=checkbox]):not([type=radio]), [role=option], .option')]
    .filter((e) => e.checkVisibility())
    .filter((e) => { const r = e.getBoundingClientRect(); return r.width > 0 && (r.width < 43.5 || r.height < 43.5); })
    .filter((e) => !(e.tagName === 'INPUT' && e.closest('.ta-control')) || e.closest('.ta-control').getBoundingClientRect().height < 43.5)
    .map((e) => `${e.tagName.toLowerCase()}.${[...e.classList].join('.')} "${(e.getAttribute('aria-label') ?? e.textContent).trim().slice(0, 24)}" `
      + `${Math.round(e.getBoundingClientRect().width)}×${Math.round(e.getBoundingClientRect().height)}`));
  // text cut off: an element whose content is wider than its box (the identifier is ellipsized on purpose, its whole in its title)
  const clipped = (frame, selector) => frame.evaluate((sel) => [...document.querySelectorAll(sel)]
    .filter((e) => e.checkVisibility() && e.scrollWidth > e.clientWidth + 1).map((e) => e.textContent.trim().slice(0, 40)), selector);
  const shot = async (page, name) => {
    if (process.env.SUPPORT_SCREENSHOTS) await page.screenshot({ path: join(process.env.SUPPORT_SCREENSHOTS, `${name}.png`) });
  };

  for (const width of [320, 360, 390]) {
    it(`fits a ${width} px phone: one header row, two-line rows, 44 px targets, the peek a sheet, menus within the view`, async () => {
      // at 320 the host says nothing of touch (the coarse pointer tells), at 360 and 390 it does, with a safe area
      const context = width === 320 ? {} : { deviceCapabilities: { touch: true, hover: false }, platform: 'mobile', safeAreaInsets: { top: 0, right: 0, bottom: 34, left: 0 } };
      for (const theme of width === 360 ? ['light', 'dark'] : ['light']) {
        const { page, app, frame } = await open({ theme, width, height: 740, phone: true, scale: width === 360 ? 2 : 1, context });
        assert.equal(await app.locator('html').getAttribute('data-size'), 'phone');
        assert.equal(await app.locator('html').getAttribute('data-touch'), 'true');
        assert.equal(await overflow(frame), 0, 'the list fits the width');
        const tops = await app.locator('.toolbar > *').evaluateAll((items) => items.filter((e) => e.checkVisibility())
          .map((e) => Math.round(e.getBoundingClientRect().top + e.getBoundingClientRect().height / 2)));
        assert.ok(Math.max(...tops) - Math.min(...tops) <= 2, `the header is one row: ${tops}`);
        const first = row(app, 'regressed');
        const [title, tags] = await Promise.all([first.locator('.title').boundingBox(), first.locator('.labels').boundingBox()]);
        assert.ok(tags && tags.y > title.y + title.height - 2, 'the tags on a second line');
        assert.deepEqual(await small(frame), [], 'every control in the list is a 44 px target');
        if (width === 360) await shot(page, `phone-list-${theme}`);

        // the search: a button, then a full-width row under the header
        await app.getByRole('button', { name: 'Search', exact: true }).tap();
        const search = app.locator('#search');
        await search.fill('socket');
        assert.deepEqual(await shown(app), [id.regressed]);
        assert.equal(await overflow(frame), 0, 'the search row fits');
        await search.press('Escape');
        assert.equal(await app.locator('.search').isVisible(), false, 'Escape closes it');

        // the filter menu, and chips that scroll in their own row
        await app.getByRole('button', { name: 'Filter', exact: true }).tap();
        await app.locator('.menu--filter').waitFor();
        const menu = await app.locator('.menu--filter').boundingBox();
        assert.ok(menu.x >= 0 && menu.x + menu.width <= width, 'the menu is within the view');
        assert.equal(await overflow(frame), 0, 'the menu fits the width');
        const done = await app.locator('.menu--filter').getByRole('button', { name: 'Done' }).boundingBox();
        assert.ok(done.y + done.height <= menu.y + menu.height + 1, 'the menu\'s Done is in reach, not below its fold');
        assert.deepEqual(await small(frame), [], 'every option is a 44 px target');
        if (width === 360) await shot(page, `phone-filter-menu-${theme}`);
        for (const name of ['tooling', 'examples']) await app.locator('.menu--filter .option', { hasText: new RegExp(`^${name}$`) }).first().tap();
        await app.getByRole('button', { name: 'Done' }).tap();
        assert.ok(await app.locator('.chip').count() >= 3);
        assert.equal(await overflow(frame), 0, 'the chips scroll in their row, not the view');
        await app.getByRole('button', { name: 'Clear', exact: true }).tap();

        // More: the display options and the rest of the wide header
        await app.getByRole('button', { name: 'More', exact: true }).tap();
        await app.getByRole('menuitem', { name: 'Display…' }).tap();
        await app.locator('.menu--display').waitFor();
        assert.equal(await overflow(frame), 0, 'the display menu fits');
        await app.getByRole('button', { name: 'Done' }).tap();

        // a tap opens the issue as a sheet; "← Issues" closes it
        await row(app, 'fresh').tap();
        await app.locator('.peek .ident', { hasText: id.fresh }).waitFor();
        await app.locator('.peek .props').waitFor();
        assert.equal(await app.locator('.pane--list').isVisible(), false, 'the sheet covers the list');
        assert.equal(await overflow(frame), 0, 'the sheet fits the width');
        assert.deepEqual(await clipped(frame, '.peek-title, .props dd, .peek .prose, .peek .code, .actions .btn'), [], 'no text cut off');
        const editable = app.locator('#title-edit');
        assert.ok(await editable.evaluate((t) => t.scrollHeight <= t.clientHeight + 1 && t.scrollWidth <= t.clientWidth + 1), 'the editable title wraps, whole');
        assert.ok(await editable.evaluate((t) => t.getBoundingClientRect().height > 40), 'on two lines at least, at this width');
        const repository = await app.locator('.props dd .nowrap').evaluateAll((parts) => parts.map((p) => p.textContent));
        assert.deepEqual(repository, ['tinyactors-dev/', 'tinyactors-napi'], 'the repository breaks only after its "/"');
        assert.deepEqual(await small(frame), [], 'every control in the sheet is a 44 px target');
        if (width === 360) await shot(page, `phone-peek-${theme}`);
        await app.getByRole('button', { name: 'Back to the issues' }).tap();
        assert.equal(await app.locator('.peek').count(), 0);
        assert.equal(await app.locator('.pane--list').isVisible(), true);
        await page.context().close();
      }
    });
  }
});

