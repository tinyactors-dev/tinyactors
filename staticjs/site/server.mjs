// Serves the built site (.vitepress/dist) with every route also as markdown:
//
//   GET /guide/language                       → HTML
//   GET /guide/language.md                    → markdown
//   GET /guide/language  Accept: text/markdown → markdown (Vary: Accept)
//   GET /llms.txt, /llms-full.txt             → the agents' entry points
//
// HTML answers carry `Link: </guide/language.md>; rel="alternate";
// type="text/markdown"`. No dependencies: node server.mjs [--port 4173].
import { createReadStream, existsSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { markdownURL, prefersMarkdown } from './.vitepress/agents.js';

const root = resolve(fileURLToPath(new URL('.', import.meta.url)), '.vitepress/dist');
const portFlag = process.argv.indexOf('--port');
const port = Number(portFlag > 0 ? process.argv[portFlag + 1] : process.env.PORT ?? 4173);

const types = {
  '.html': 'text/html; charset=utf-8',
  '.md': 'text/markdown; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.woff2': 'font/woff2',
  '.woff': 'font/woff',
  '.wasm': 'application/wasm',
  '.gz': 'application/gzip',
};

/** The file under dist for a path, or null: no escaping the root. */
function file(path) {
  const full = normalize(join(root, path));
  if (!full.startsWith(root)) return null;
  return existsSync(full) && statSync(full).isFile() ? full : null;
}

/** The route a request names, without ".md", ".html" or a trailing "index". */
function routeOf(pathname) {
  let route = pathname.replace(/\.(md|html)$/, '').replace(/\/index$/, '/');
  if (route.length > 1 && route.endsWith('/')) route = route.slice(0, -1);
  return route || '/';
}

function send(res, path, status = 200, headers = {}) {
  res.writeHead(status, {
    'Content-Type': types[extname(path)] ?? 'application/octet-stream',
    'Cache-Control': path.includes('/assets/') ? 'public, max-age=31536000, immutable' : 'no-cache',
    ...(path.startsWith(join(root, 'download/'))
      ? { 'Content-Disposition': `attachment; filename="${path.split('/').pop()}"`, ...(extname(path) ? {} : { 'Content-Type': 'text/plain; charset=utf-8' }) }
      : {}),
    ...headers,
  });
  createReadStream(path)
    .on('error', () => res.destroy())
    .pipe(res);
}

/** A 404: the site's page for a browser, plain text for markdown or when the page is missing. */
function notFound(res, markdown) {
  const page = !markdown && file('/404.html');
  if (page) return send(res, page, 404, { Vary: 'Accept' });
  res.writeHead(404, { 'Content-Type': `${markdown ? 'text/markdown' : 'text/plain'}; charset=utf-8`, Vary: 'Accept' });
  res.end('# Not found\n\nNo page here. Every page is listed in /llms.txt.\n');
}

const server = createServer((req, res) => {
  const url = new URL(req.url ?? '/', 'http://localhost');
  let pathname;
  try {
    pathname = decodeURIComponent(url.pathname);
  } catch {
    res.writeHead(400).end('bad request');
    return;
  }
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405, { Allow: 'GET, HEAD' }).end();
    return;
  }

  // A file as named: assets, the wasm module, llms.txt, a page's .md, and
  // the release's downloads (SHA256SUMS has no extension).
  const direct = !pathname.endsWith('/') && (extname(pathname) || pathname.startsWith('/download/')) && file(pathname);
  if (direct) return send(res, direct);

  const route = routeOf(pathname);
  const markdown = file(markdownURL(route)) ?? file(markdownURL(`${route}/`));
  if (pathname.endsWith('.md') || prefersMarkdown(req.headers.accept)) {
    if (markdown) return send(res, markdown, 200, { Vary: 'Accept' });
    return notFound(res, true);
  }
  const html = file(route === '/' ? '/index.html' : `${route}.html`) ?? file(`${route}/index.html`);
  const alternate = markdown ? { Link: `<${markdown.slice(root.length)}>; rel="alternate"; type="text/markdown"` } : {};
  if (html) return send(res, html, 200, { Vary: 'Accept', ...alternate });
  return notFound(res, false);
});
server.listen(port, () => {
  console.log(`staticjs site: http://localhost:${server.address().port} (markdown at any route + .md, or with Accept: text/markdown)`);
});
