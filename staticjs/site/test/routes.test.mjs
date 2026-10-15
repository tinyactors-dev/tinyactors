// The built site's routes: every page as HTML, as markdown at its path plus
// ".md" and by Accept negotiation, and llms.txt listing pages that exist.
// Run by `mise run site:test` after a build: node --test test/
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const site = fileURLToPath(new URL('..', import.meta.url));
let server;
let base;

before(async () => {
  server = spawn(process.execPath, ['server.mjs', '--port', '0'], { cwd: site, stdio: ['ignore', 'pipe', 'inherit'] });
  base = await new Promise((resolve, reject) => {
    server.stdout.on('data', (chunk) => {
      const m = /http:\/\/localhost:(\d+)/.exec(String(chunk));
      if (m) resolve(`http://localhost:${m[1]}`);
    });
    server.on('exit', (code) => reject(new Error(`server exited ${code}`)));
  });
});
after(() => server?.kill());

const get = (path, headers = {}) => fetch(base + path, { headers });

async function pages() {
  const text = await (await get('/llms.txt')).text();
  return [...text.matchAll(/^- \[[^\]]+\]\((\/[^)]+\.md)\)/gm)].map((m) => m[1]);
}

test('llms.txt lists every page, as markdown that exists', async () => {
  const res = await get('/llms.txt');
  assert.equal(res.status, 200);
  assert.match(res.headers.get('content-type'), /^text\/plain/);
  const text = await res.text();
  assert.match(text, /^# staticjs\n\n> /);
  const links = await pages();
  assert.ok(links.length >= 9, `${links.length} pages`);
  for (const link of links) {
    const page = await get(link);
    assert.equal(page.status, 200, link);
    assert.match(page.headers.get('content-type'), /^text\/markdown/, link);
    const body = await page.text();
    assert.match(body, /^# /, `${link} starts with a title`);
    assert.doesNotMatch(body, /<[A-Z][A-Za-z]+[\s/>]|```js live|<script/, `${link} has no Vue or live fences`);
  }
  assert.equal((await get('/llms-full.txt')).status, 200);
});

test('every route is HTML by default, with its markdown as an alternate', async () => {
  for (const md of await pages()) {
    const route = md === '/index.md' ? '/' : md.replace(/\.md$/, '');
    const res = await get(route, { Accept: 'text/html,application/xhtml+xml,*/*;q=0.8' });
    assert.equal(res.status, 200, route);
    assert.match(res.headers.get('content-type'), /^text\/html/, route);
    assert.equal(res.headers.get('vary'), 'Accept', route);
    assert.equal(res.headers.get('link'), `<${md}>; rel="alternate"; type="text/markdown"`, route);
    assert.match(await res.text(), new RegExp(`<link rel="alternate" type="text/markdown" href="${md}"`), route);
  }
});

test('Accept: text/markdown gets markdown at the same route', async () => {
  for (const accept of ['text/markdown', 'text/markdown, text/html;q=0.9', 'text/x-markdown']) {
    const res = await get('/guide/language', { Accept: accept });
    assert.equal(res.status, 200, accept);
    assert.match(res.headers.get('content-type'), /^text\/markdown/, accept);
    assert.match(await res.text(), /^# The language/, accept);
  }
  const html = await get('/guide/language', { Accept: 'text/html, text/markdown;q=0.5' });
  assert.match(html.headers.get('content-type'), /^text\/html/);
});

test('a live sample becomes code and its result in markdown', async () => {
  const text = await (await get('/guide/language.md')).text();
  assert.match(text, /```js\n\[3, 1, 2\]|Result: `/);
  assert.match(text, /\nResult: `[^`]+` \(\d+ units of fuel\)\n/);
  assert.match(await (await get('/guide/faults.md')).text(), /\nFault at \d+:\d+: /);
});

test('links between pages point at markdown in markdown', async () => {
  const text = await (await get('/guide/getting-started.md')).text();
  assert.match(text, /\]\(\.\/language\.md\)|\]\(\/guide\/language\.md\)/);
});

test('a missing page is a 404, in either form', async () => {
  assert.equal((await get('/guide/nothing')).status, 404);
  const md = await get('/guide/nothing.md');
  assert.equal(md.status, 404);
  assert.match(md.headers.get('content-type'), /^text\/markdown/);
  assert.equal((await get('/guide/nothing', { Accept: 'text/markdown' })).status, 404);
  assert.equal((await get('/../../etc/passwd')).status, 404);
});

test('section indexes, the 404 page and the generated markdown', async () => {
  for (const path of ['/guide/', '/explore/', '/guide', '/explore']) assert.equal((await get(path)).status, 200, path);
  assert.match(await (await get('/guide/', { Accept: 'text/markdown' })).text(), /^# Guide/);
  const missing = await get('/nowhere');
  assert.equal(missing.status, 404);
  assert.match(missing.headers.get('content-type'), /^text\/html/);
  assert.doesNotMatch(await missing.text(), /404\.md/);
  const pipeline = await (await get('/explore/pipeline.md')).text();
  assert.match(pipeline, /## A worked example/);
  assert.match(pipeline, /shape `PLUCK`/);
  const examples = await (await get('/guide/examples.md')).text();
  assert.match(examples, /```c\n[\s\S]*#include "staticjs.h"/);
  assert.doesNotMatch(examples, /^<<<|^:::/m);
  const full = await (await get('/llms-full.txt')).text();
  assert.match(full, /^Source: \/guide\/language\.md$/m);
});

test('Getting started offers the release, every file a download from this site', async (t) => {
  const html = await (await get('/guide/getting-started')).text();
  const links = [...html.matchAll(/<a href="(\/download\/[^"]+)" download>/g)].map((m) => m[1]);
  if (links.length === 0) {
    assert.match(html, /mise run site:release/);
    t.skip('this build has no release (mise run site:release)');
    return;
  }
  assert.ok(links.some((l) => l.endsWith('/SHA256SUMS')), 'SHA256SUMS is offered');
  for (const link of links) {
    const res = await get(link);
    assert.equal(res.status, 200, link);
    assert.match(res.headers.get('content-disposition'), /^attachment; filename="/, link);
    assert.ok((await res.arrayBuffer()).byteLength > 0, link);
  }
  const markdown = await (await get('/guide/getting-started.md')).text();
  for (const link of links) assert.ok(markdown.includes(link), `${link} in the markdown`);
});
