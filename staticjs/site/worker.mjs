// Serves the built site (.vitepress/dist) on Cloudflare Workers, as
// server.mjs does locally: every route as HTML, and as markdown by ".md" or
// `Accept: text/markdown`, with `Vary` and `Link` headers. The files are the
// Worker's static assets (wrangler.jsonc), fetched by exact path through the
// ASSETS binding; this script decides which one a request names.
import { markdownURL, prefersMarkdown } from './.vitepress/agents.js';

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
};

function extname(path) {
  const match = /\.[^./]+$/.exec(path);
  return match ? match[0] : '';
}

/** The route a request names, without ".md", ".html" or a trailing "index". */
function routeOf(pathname) {
  let route = pathname.replace(/\.(md|html)$/, '').replace(/\/index$/, '/');
  if (route.length > 1 && route.endsWith('/')) route = route.slice(0, -1);
  return route || '/';
}

export default {
  async fetch(request, env) {
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return new Response(null, { status: 405, headers: { Allow: 'GET, HEAD' } });
    }
    const url = new URL(request.url);
    let pathname;
    try {
      pathname = decodeURIComponent(url.pathname);
    } catch {
      return new Response('bad request', { status: 400 });
    }

    /** The asset at a path, or null. */
    const file = async (path) => {
      const response = await env.ASSETS.fetch(new URL(encodeURI(path), url), {
        method: request.method,
        headers: request.headers,
      });
      return response.status === 200 || response.status === 304 ? { path, response } : null;
    };
    const send = (found, status = 200, headers = {}) => {
      const out = new Headers(found.response.headers);
      out.set('Content-Type', types[extname(found.path)] ?? 'application/octet-stream');
      out.set('Cache-Control', found.path.includes('/assets/') ? 'public, max-age=31536000, immutable' : 'no-cache');
      for (const [name, value] of Object.entries(headers)) out.set(name, value);
      return new Response(found.response.body, { status: found.response.status === 304 ? 304 : status, headers: out });
    };
    const notFound = async (markdown) => {
      const page = !markdown && (await file('/404.html'));
      if (page) return send(page, 404, { Vary: 'Accept' });
      return new Response('# Not found\n\nNo page here. Every page is listed in /llms.txt.\n', {
        status: 404,
        headers: { 'Content-Type': `${markdown ? 'text/markdown' : 'text/plain'}; charset=utf-8`, Vary: 'Accept' },
      });
    };

    // A file as named: assets, the wasm module, llms.txt, a page's .md.
    const direct = !pathname.endsWith('/') && extname(pathname) && (await file(pathname));
    if (direct) return send(direct);

    const route = routeOf(pathname);
    const markdown = (await file(markdownURL(route))) ?? (await file(markdownURL(`${route}/`)));
    if (pathname.endsWith('.md') || prefersMarkdown(request.headers.get('accept'))) {
      if (markdown) return send(markdown, 200, { Vary: 'Accept' });
      return notFound(true);
    }
    const html = (await file(route === '/' ? '/index.html' : `${route}.html`)) ?? (await file(`${route}/index.html`));
    const alternate = markdown ? { Link: `<${markdown.path}>; rel="alternate"; type="text/markdown"` } : {};
    if (html) return send(html, 200, { Vary: 'Accept', ...alternate });
    return notFound(false);
  },
};
