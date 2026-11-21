// A ticket: what the site's Worker vouches for, so the presence server
// needs no GeoIP database. The Worker answers GET /presence/ticket with the
// visitor's country (Cloudflare's request.cf.country), a time it expires,
// and a nonce, signed with HMAC-SHA-256 under TICKET_KEY, which the Worker
// and the server share. The server accepts each nonce once, before it
// expires. WebCrypto only, so the same code runs in the Worker, in Node.js
// and in a browser.

export const LIFETIME = 60_000; // ms a ticket stays good

const encoder = new TextEncoder();
const base64url = (bytes) => btoa(String.fromCharCode(...new Uint8Array(bytes))).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
const unbase64url = (text) => Uint8Array.from(atob(text.replaceAll('-', '+').replaceAll('_', '/')), (c) => c.charCodeAt(0));

async function hmac(secret) {
  return crypto.subtle.importKey('raw', encoder.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
}

/** A signed ticket for `country` (two letters; anything else is kept as null). */
export async function issue(secret, country, now = Date.now()) {
  const nonce = base64url(crypto.getRandomValues(new Uint8Array(12)));
  const body = base64url(encoder.encode(JSON.stringify({ country: /^[A-Z]{2}$/.test(country ?? '') ? country : null, exp: now + LIFETIME, nonce })));
  const signature = await crypto.subtle.sign('HMAC', await hmac(secret), encoder.encode(body));
  return `${body}.${base64url(signature)}`;
}

/** The ticket's `{country, exp, nonce}` when its signature holds and it has not expired, else null. */
export async function check(secret, ticket, now = Date.now()) {
  if (typeof ticket !== 'string' || ticket.length > 512) return null;
  const [body, signature] = ticket.split('.');
  if (!body || !signature) return null;
  let ok = false;
  try { ok = await crypto.subtle.verify('HMAC', await hmac(secret), unbase64url(signature), encoder.encode(body)); } catch { return null; }
  if (!ok) return null;
  const claims = JSON.parse(new TextDecoder().decode(unbase64url(body)));
  return claims.exp > now ? claims : null;
}
