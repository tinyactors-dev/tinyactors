// The issue list app in Chromium, under a fake host: the host answers
// ui/initialize with a light or a dark theme, sends the tool's input and
// result as a host does after calling known_issues, and passes the app's
// tools/call requests to this process's service (the stand-ins), as the
// signed-in maintainer. The app is the file the server serves.
//
// SUPPORT_SCREENSHOTS=<dir> writes the list and an issue in both themes.

import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { stack } from './stack.mjs';
import { APP } from '../lib/app/index.mjs';
import { toolResult } from '../lib/http.mjs';

let chromium = null;
try { ({ chromium } = createRequire(import.meta.url)('playwright')); } catch { /* reported by the skip below */ }

const napi = 'tinyactors-dev/tinyactors-napi';
const fresh = (component, title, body = title) => ({ repo: napi, component, kind: 'tooling', minutes: 12, title, body });

// The host page: an iframe of the app and the host's side of the protocol.
const HOST = `<!doctype html><html><body style="margin:0">
<iframe id="app" style="border:0;width:100%;height:900px"></iframe>
<script>
  const frame = document.getElementById('app');
  window.calls = [];
  window.addEventListener('message', async (event) => {
    if (event.source !== frame.contentWindow) return;
    const m = event.data, reply = (result) => frame.contentWindow.postMessage({ jsonrpc: '2.0', id: m.id, result }, '*');
    if (m.method === 'ui/initialize') return reply({ protocolVersion: '2026-01-26', hostInfo: { name: 'fake-host', version: '1' },
      hostCapabilities: { serverTools: {} }, hostContext: { theme: window.theme, displayMode: 'inline', availableDisplayModes: ['inline'] } });
    if (m.method === 'ui/notifications/initialized') {
      frame.contentWindow.postMessage({ jsonrpc: '2.0', method: 'ui/notifications/tool-input', params: { arguments: window.toolArguments } }, '*');
      const result = await window.callTool(window.toolName, window.toolArguments);
      return frame.contentWindow.postMessage({ jsonrpc: '2.0', method: 'ui/notifications/tool-result', params: result }, '*');
    }
    if (m.method === 'ui/notifications/size-changed') { window.size = m.params; return; }
    if (m.method === 'tools/call') { window.calls.push(m.params); return reply(await window.callTool(m.params.name, m.params.arguments)); }
    if (m.id !== undefined) reply({});
  });
</script></body></html>`;

describe('the issue list app', { skip: chromium ? false : 'Playwright is not installed here' }, () => {
  let s, bob, ann, browser, fixedID, newID;
  before(async () => {
    s = await stack();
    bob = (await s.signIn('code-bob')).access_token;
    ann = (await s.signIn('code-ann')).access_token;
    newID = (await s.tool(ann, 'report_issue', fresh('examples', 'mise run examples fails before mise run build',
      'cannot find build/tinyactors.node when the examples run before the addon is built'))).id;
    await s.tool(ann, 'report_issue', fresh('src', 'Host.restore loses the clock offset'));
    fixedID = (await s.tool(ann, 'report_issue', fresh('docs', 'the README links to a guide that is missing'))).id;
    await s.tool(ann, 'attach_workaround', { id: newID, text: 'run `mise run build` in tinyactors-napi first' });
    s.github.commit(1001, 'm2', `docs(napi): the stores guide\n\nFixes-issue: ${fixedID}`);
    await s.advance(600_000);
    browser = await chromium.launch();
  });
  after(async () => { await browser?.close(); await s?.support.close(); });

  // A page with the app under the fake host, for `token`'s user, after `tool` was called with `args`.
  async function open({ theme = 'light', token = bob, tool = 'known_issues', args = {} } = {}) {
    const page = await browser.newPage({ viewport: { width: 760, height: 900 }, deviceScaleFactor: 1, colorScheme: theme });
    await page.exposeFunction('callTool', async (name, input) => {
      const caller = token ? await s.gateway.caller(token) : null;
      const result = await toolResult(s.gateway, caller, name, input);
      await s.idle();
      return result;
    });
    await page.setContent(HOST);
    await page.evaluate(([t, name, input]) => { window.theme = t; window.toolName = name; window.toolArguments = input; }, [theme, tool, args]);
    await page.evaluate((html) => { document.getElementById('app').srcdoc = html; }, APP.html);
    const app = page.frameLocator('#app');
    return { page, app };
  }
  const shoot = async (page, name) => {
    if (process.env.SUPPORT_SCREENSHOTS) await page.screenshot({ path: join(process.env.SUPPORT_SCREENSHOTS, `${name}.png`), fullPage: true });
  };

  it('shows the open issues the tool found, in the host\'s theme, with the design system\'s colours', async () => {
    for (const theme of ['light', 'dark']) {
      const { page, app } = await open({ theme });
      await app.locator('.row').first().waitFor();
      assert.equal(await app.locator('.row').count(), 2, 'the two open issues; the fixed one is not open');
      assert.equal(await app.locator('html').getAttribute('data-theme'), theme);
      const background = await app.locator('body').evaluate((b) => getComputedStyle(b).backgroundColor);
      assert.equal(background, theme === 'dark' ? 'rgb(24, 18, 13)' : 'rgb(244, 238, 226)', 'umber-950 or vellum-100');
      assert.ok(await page.evaluate(() => window.size?.height > 0), 'the app told the host its size');
      await shoot(page, `issues-list-${theme}`);
      await app.locator(`.row[data-id="${newID}"]`).click();
      await app.getByRole('heading', { name: 'mise run examples fails before mise run build' }).waitFor();
      await app.getByText('run `mise run build` in tinyactors-napi first').waitFor();
      await shoot(page, `issues-detail-${theme}`);
      await page.close();
    }
  });

  it('opens the issue get_issue was called for', async () => {
    const { page, app } = await open({ tool: 'get_issue', args: { id: fixedID } });
    await app.getByRole('heading', { name: 'the README links to a guide that is missing' }).waitFor();
    assert.equal(await app.locator('.badge').textContent(), 'fixed');
    await page.close();
  });

  it('filters, opens, triages, closes and deletes through the server\'s tools', async () => {
    const { page, app } = await open();
    await app.locator('.row').first().waitFor();
    await app.getByLabel('State').selectOption('all');
    await app.locator(`.row[data-id="${fixedID}"]`).waitFor();
    assert.equal(await app.locator('.row').count(), 3);
    await app.getByLabel('Repository').selectOption(napi);
    await app.getByLabel('Component').selectOption('docs');
    await app.locator('.row').first().waitFor();
    assert.equal(await app.locator('.row').count(), 1);
    // close the fixed one
    await app.locator(`.row[data-id="${fixedID}"]`).click();
    await app.getByRole('button', { name: 'Close' }).click();
    await app.locator('.badge', { hasText: 'closed' }).waitFor();
    assert.match(await app.locator('#status').textContent(), new RegExp(`^Closed ${fixedID}`));
    // triage the new one: kind and title
    await app.getByRole('button', { name: '← Issues' }).click();
    await app.getByLabel('Component').selectOption('');
    await app.getByLabel('State').selectOption('open');
    await app.locator(`.row[data-id="${newID}"]`).click();
    await app.getByLabel('Kind').selectOption('tooling');
    await app.getByLabel('A better title').fill('mise run examples needs the addon built first');
    await app.getByRole('button', { name: 'Triage' }).click();
    await app.getByRole('heading', { name: 'mise run examples needs the addon built first' }).waitFor();
    assert.equal(await app.locator('.badge').textContent(), 'triaged');
    // delete it, after confirming
    await app.getByRole('button', { name: 'Delete…' }).click();
    await app.getByLabel('Reason, for the ledger').fill('a test report');
    await app.getByRole('button', { name: 'Delete for good' }).click();
    await app.locator('.row').first().waitFor();
    assert.equal(await app.locator(`.row[data-id="${newID}"]`).count(), 0);
    assert.match((await s.tool(null, 'get_issue', { id: newID })).reason, /was deleted/);
    const names = await page.evaluate(() => window.calls.map((c) => c.name));
    for (const name of ['list_projects', 'whoami', 'known_issues', 'get_issue', 'close', 'triage', 'delete_issue'])
      assert.ok(names.includes(name), `the app called ${name}`);
    await page.close();
  });

  it('offers a reporter no maintainer\'s actions, and anyone signed in the report form', async () => {
    const { page, app } = await open({ token: ann });
    await app.locator('.row').first().click();
    await app.getByRole('button', { name: 'Attach workaround' }).waitFor();
    assert.equal(await app.getByRole('button', { name: 'Delete…' }).count(), 0);
    await app.getByRole('button', { name: '← Issues' }).click();
    await app.getByRole('button', { name: 'Report issue' }).click();
    await app.getByLabel('Repository').selectOption(napi);
    await app.getByLabel('Component').selectOption('lib');
    await app.getByLabel('What went wrong, in a line').fill('lib exports nothing for bun');
    await app.getByRole('button', { name: 'Report', exact: true }).click();
    await app.getByRole('heading', { name: 'lib exports nothing for bun' }).waitFor();
    assert.match(await app.locator('#status').textContent(), /^Filed tinyactors:tinyactors-napi:\d+/);
    await page.close();
  });
});
