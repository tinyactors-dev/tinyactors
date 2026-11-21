// The site's Worker (wrangler.jsonc): the build's static assets, and one
// route of code in front of them, GET /presence/ticket, which vouches for
// the visitor's country to the presence server (presence/ticket.mjs,
// docs/presence.md). Cloudflare's request.cf.country is the country;
// TICKET_KEY is a Worker secret that the server shares (mise run
// presence:key). Every other path is served by the assets alone.
import { issue } from './presence/ticket.mjs';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/presence/ticket') {
      if (request.method !== 'GET') return new Response(null, { status: 405, headers: { allow: 'GET' } });
      if (!env.TICKET_KEY) return new Response(null, { status: 503 });
      const ticket = await issue(env.TICKET_KEY, request.cf?.country ?? null);
      return Response.json({ ticket }, { headers: { 'cache-control': 'no-store' } });
    }
    return env.ASSETS.fetch(request);
  },
};
