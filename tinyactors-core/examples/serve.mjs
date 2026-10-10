#!/usr/bin/env node
/**
 * A static server for the browser examples (correct MIME types for .mjs and
 * .wasm). Serves examples/ and the web binding from the repository.
 *
 *   mise run example:ui [--port 8000]   (or: mise run web:build && node examples/serve.mjs)
 *   open http://localhost:8000/examples/ui-elements/
 */
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../', import.meta.url));
const arg = process.argv.indexOf('--port');
const PORT = Number(arg > 0 ? process.argv[arg + 1] : process.env.PORT || 8000);
const MIME = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.json': 'application/json' };
const PUBLIC = ['/examples/', '/bindings/web/', '/bindings/shared/'];

http
  .createServer(async (req, res) => {
    let path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (path === '/') {
      res.writeHead(302, { location: '/examples/ui-elements/' });
      return res.end();
    }
    if (path.endsWith('/')) path += 'index.html';
    const file = normalize(join(ROOT, path));
    if (!PUBLIC.some((p) => path.startsWith(p)) || !file.startsWith(ROOT)) {
      res.writeHead(404);
      return res.end('not found');
    }
    try {
      const body = await readFile(file);
      res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream', 'cache-control': 'no-cache' });
      res.end(body);
    } catch {
      res.writeHead(404);
      res.end(path.endsWith('.wasm') ? 'not built: run `mise run web:build`' : 'not found');
    }
  })
  .listen(PORT, function () {
    console.log(`examples: http://localhost:${this.address().port}/examples/ui-elements/`);
  });
