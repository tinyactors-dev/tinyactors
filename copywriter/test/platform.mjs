// What the Playwright tests of the extension and the widget share: a
// static server on a free port (the fixture page, and any directory given),
// Gemini answered by a route, a text selection in a page.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, extname, join, normalize } from 'node:path';

const require = createRequire(import.meta.url);
export const { chromium } = require('playwright');

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
export const FIXTURE = join(ROOT, 'test/fixtures/page.html');
const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm', '.zip': 'application/zip', '.css': 'text/css', '.woff2': 'font/woff2', '.svg': 'image/svg+xml', '.xml': 'application/xml', '.xsd': 'application/xml',
};

/**
 * Serves `routes`: a map of URL path prefix → a directory, or of exact
 * path → a file. A directory's path serves its index.html; /llms.txt,
 * unless routed, is an empty site map. Port 0.
 */
export function serve(routes) {
  const server = createServer(async (request, response) => {
    const path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    for (const [prefix, target] of Object.entries(routes)) {
      if (path !== prefix && !(prefix.endsWith('/') && path.startsWith(prefix))) continue;
      let file = prefix.endsWith('/') ? join(target, normalize(path.slice(prefix.length)).replace(/^(\.\.[/\\])+/, '')) : target;
      if (file.endsWith('/') || path.endsWith('/')) file = join(file, 'index.html');
      try {
        const body = await readFile(file);
        response.writeHead(200, { 'content-type': TYPES[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
        response.end(body);
      } catch {
        response.writeHead(404).end();
      }
      return;
    }
    // a site with no pages in its map, unless a route serves one: the panel asks every origin for it
    if (path === '/llms.txt') { response.writeHead(200, { 'content-type': 'text/plain' }).end('# Fixture\n'); return; }
    response.writeHead(404).end();
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

export const origin = (server) => `http://127.0.0.1:${server.address().port}`;

/** A generateContent body: text, then function calls. */
export function geminiReply(text, calls = []) {
  return {
    candidates: [{
      content: {
        role: 'model',
        parts: [...(text ? [{ text }] : []), ...calls.map(([name, args]) => ({ functionCall: { name, args } }))],
      },
    }],
  };
}

export const REWRITE = 'Run millions of actors in one process.';

/**
 * Answers Gemini in `context`: models.list with two models, every
 * generateContent with one rewrite. `requests` collects their URLs.
 */
export async function routeGemini(context) {
  const requests = [];
  await context.route('https://generativelanguage.googleapis.com/**', async (route) => {
    const url = route.request().url();
    const cors = { 'access-control-allow-origin': '*', 'access-control-allow-headers': '*', 'access-control-allow-methods': 'GET, POST' };
    if (route.request().method() === 'OPTIONS') return route.fulfill({ status: 204, headers: cors });
    requests.push(url);
    const body = url.includes(':generateContent')
      ? geminiReply('Say what the reader gets.', [['propose_rewrite', { replacement: REWRITE, rationale: 'A measured claim.' }]])
      : { models: [
        { name: 'models/gemini-4-hydrogen', displayName: 'Gemini 4 Hydrogen', supportedGenerationMethods: ['generateContent'] },
        { name: 'models/gemini-4-carbon', displayName: 'Gemini 4 Carbon', supportedGenerationMethods: ['generateContent'] },
      ] };
    await route.fulfill({ status: 200, contentType: 'application/json', headers: cors, body: JSON.stringify(body) });
  });
  return requests;
}

/** Selects the first occurrence of `text` in the element matching `selector`. */
export function selectText(page, selector, text) {
  return page.evaluate(([selector, text]) => {
    const el = document.querySelector(selector);
    const walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      const at = node.data.indexOf(text);
      if (at < 0) continue;
      const range = document.createRange();
      range.setStart(node, at);
      range.setEnd(node, at + text.length);
      const selection = getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      return true;
    }
    throw new Error(`not in ${selector}: ${text}`);
  }, [selector, text]);
}
