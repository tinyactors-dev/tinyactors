// A static server for the panel's preview: bundles tools/preview.mjs with
// the pinned esbuild on every request (in memory, nothing written) and
// serves tools/preview.html and the design system's fonts.
//
//   node tools/serve.mjs [port]     → http://localhost:<port>/  (default 8317)
//
// The test (test/ui-panel.spec.mjs) imports startPreviewServer and listens
// on port 0.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, basename } from 'node:path';
import * as esbuild from 'esbuild';

const here = dirname(fileURLToPath(import.meta.url));
const fonts = join(here, '../../.agents/skills/tinyactors-design/assets/fonts');

export async function bundle() {
  const result = await esbuild.build({
    entryPoints: [join(here, 'preview.mjs')],
    bundle: true, format: 'esm', write: false, minify: false, sourcemap: 'inline', target: 'es2022',
    logLevel: 'silent',
  });
  return result.outputFiles[0].contents;
}

export function startPreviewServer(port = 0) {
  const server = createServer(async (request, response) => {
    const path = new URL(request.url, 'http://localhost').pathname;
    try {
      if (path === '/' || path === '/preview.html') {
        response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        response.end(await readFile(join(here, 'preview.html')));
      } else if (path === '/preview.js') {
        response.writeHead(200, { 'content-type': 'text/javascript; charset=utf-8', 'cache-control': 'no-store' });
        response.end(await bundle());
      } else if (path.startsWith('/fonts/')) {
        response.writeHead(200, { 'content-type': 'font/woff2' });
        response.end(await readFile(join(fonts, basename(path))));
      } else {
        response.writeHead(404).end();
      }
    } catch (error) {
      response.writeHead(500, { 'content-type': 'text/plain' }).end(String(error?.stack ?? error));
    }
  });
  return new Promise((resolve) => server.listen(port, '127.0.0.1', () => resolve(server)));
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const server = await startPreviewServer(Number(process.argv[2] ?? 8317));
  console.log(`panel preview: http://127.0.0.1:${server.address().port}/?layout=sidebar`);
}
