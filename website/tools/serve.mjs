// Serves a built site as Cloudflare's static assets do (wrangler.jsonc):
// _redirects' static rules first, a directory as its index.html, "/x" as
// x.html when that exists, "/x" redirected to "/x/" when x/index.html
// exists, and 404.html with a 404 for what is missing. No dependencies.
// With TICKET_KEY set it also answers /presence/ticket as the Worker does
// (worker.mjs), for a presence server run here (COUNTRY, default EE).
//
//   node tools/serve.mjs build/site 4321

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { ranged } from '../range.mjs';
import { extname, join, normalize } from 'node:path';

const [root = 'build/site', port = '4321'] = process.argv.slice(2);

const types = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.xml': 'application/xml; charset=utf-8',
  '.xsd': 'application/xml; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.mp4': 'video/mp4',
  '.webm': 'video/webm',
  '.jpg': 'image/jpeg',
};

// _redirects: "from to status", one a line, every rule static here.
const redirects = new Map(
  (await readFile(join(root, '_redirects'), 'utf8').catch(() => ''))
    .split('\n')
    .map((line) => line.trim().split(/\s+/))
    .filter((rule) => rule.length >= 2 && rule[0].startsWith('/'))
    .map(([from, to, status = '302']) => [from, { to, status: Number(status) }]),
);

const isFile = async (path) => (await stat(path).catch(() => null))?.isFile() ?? false;

createServer(async (request, response) => {
  const path = normalize(decodeURIComponent(new URL(request.url, 'http://localhost').pathname));
  if (path === '/presence/ticket') {
    if (!process.env.TICKET_KEY) return response.writeHead(503).end();
    const { issue } = await import('../presence/ticket.mjs');
    response.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    return response.end(JSON.stringify({ ticket: await issue(process.env.TICKET_KEY, process.env.COUNTRY ?? 'EE') }));
  }
  if (path.includes('..')) {
    response.writeHead(400).end();
    return;
  }
  const redirect = redirects.get(path);
  if (redirect) return response.writeHead(redirect.status, { location: redirect.to }).end();
  let file = join(root, path.endsWith('/') ? join(path, 'index.html') : path);
  if (!path.endsWith('/') && !(await isFile(file)) && (await isFile(`${file}.html`))) file = `${file}.html`;
  let status = 200;
  if (!path.endsWith('/') && (await isFile(join(root, path, 'index.html')))) {
    response.writeHead(307, { location: `${path}/` }).end();
    return;
  }
  if (!(await isFile(file))) {
    file = join(root, '404.html');
    status = 404;
  }
  const type = types[extname(file)] ?? 'application/octet-stream';
  if (status === 404) return response.writeHead(404, { 'content-type': type }).end(await readFile(file));
  const part = ranged(await readFile(file), request.headers.range);
  response.writeHead(part.status, { 'content-type': type, ...part.headers });
  response.end(part.body ?? undefined);
}).listen(Number(port), () => console.log(`website: http://localhost:${port}/ serves ${root}`));
