// The site's Worker (wrangler.jsonc): the build's static assets, and one
// route of code in front of them, GET /presence/ticket, which vouches for
// the visitor's country to the presence server (presence/ticket.mjs,
// docs/presence.md). Cloudflare's request.cf.country is the country;
// TICKET_KEY is a Worker secret that the server shares (mise run
// presence:key). GET /assets/videos/* answers a Range (range.mjs), which
// the assets alone do not, so a demo's video can be sought. Every other
// path is served by the assets alone.
import { issue } from './presence/ticket.mjs';
import { ranged } from './range.mjs';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/presence/ticket') {
      if (request.method !== 'GET') return new Response(null, { status: 405, headers: { allow: 'GET' } });
      if (!env.TICKET_KEY) return new Response(null, { status: 503 });
      const ticket = await issue(env.TICKET_KEY, request.cf?.country ?? null);
      return Response.json({ ticket }, { headers: { 'cache-control': 'no-store' } });
    }
    if (url.pathname.startsWith('/assets/videos/') && request.method === 'GET') {
      const asset = await env.ASSETS.fetch(new Request(url, { method: 'GET' }));
      if (!asset.ok) return asset;
      const { status, headers, body } = ranged(await asset.arrayBuffer(), request.headers.get('range'));
      const out = new Headers(asset.headers);
      out.delete('content-length');
      for (const [name, value] of Object.entries(headers)) out.set(name, value);
      return new Response(body, { status, headers: out });
    }
    return env.ASSETS.fetch(request);
  },
};
