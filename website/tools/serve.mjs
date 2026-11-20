// Serves a built site as Cloudflare's static assets do (wrangler.jsonc): a
// directory as its index.html, "/x" redirected to "/x/" when x/index.html
// exists, and 404.html with a 404 for what is missing. No dependencies.
//
//   node tools/serve.mjs build/site 4321

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
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
  '.txt': 'text/plain; charset=utf-8',
};

const isFile = async (path) => (await stat(path).catch(() => null))?.isFile() ?? false;

createServer(async (request, response) => {
  const path = normalize(decodeURIComponent(new URL(request.url, 'http://localhost').pathname));
  if (path.includes('..')) {
    response.writeHead(400).end();
    return;
  }
  let file = join(root, path.endsWith('/') ? join(path, 'index.html') : path);
  let status = 200;
  if (!path.endsWith('/') && (await isFile(join(root, path, 'index.html')))) {
    response.writeHead(307, { location: `${path}/` }).end();
    return;
  }
  if (!(await isFile(file))) {
    file = join(root, '404.html');
    status = 404;
  }
  response.writeHead(status, { 'content-type': types[extname(file)] ?? 'application/octet-stream' });
  response.end(await readFile(file));
}).listen(Number(port), () => console.log(`website: http://localhost:${port}/ serves ${root}`));
