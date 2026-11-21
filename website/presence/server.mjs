// The presence server: one tinyactors host whose actors are the visitors'
// ants (visitor.scxml, world.mjs), and a WebSocket per open tab. It runs
// on the exe VM ta-presence behind exe's HTTPS proxy, at
// wss://ants.www.tinyactors.dev (website/docs/presence.md).
//
// A tab's first message is hello: a ticket from the site's Worker (its
// country, signed) and the tab's id. A tab id already connected is refused
// (a duplicated tab copies sessionStorage); a tab id whose socket closed
// less than GRACE ago gets its ant back. Then the tab sends greet, hide,
// show and leave, and gets, ten times a second, what the world said since
// the last tick, and every two seconds the field it draws faintly.
//
// Environment: PORT (8000), TICKET_KEY (required), ORIGINS (comma-separated;
// https://www.tinyactors.dev).
import http from 'node:http';
import { readFileSync } from 'node:fs';
import { WebSocketServer } from 'ws';
import * as tinyactors from '@tinyactors/node';
import { createWorld } from './world.mjs';
import { check } from './ticket.mjs';

const PORT = Number(process.env.PORT ?? 8000);
const KEY = process.env.TICKET_KEY;
const ORIGINS = new Set((process.env.ORIGINS ?? 'https://www.tinyactors.dev').split(',').map((s) => s.trim()));
const TICK = 100, FIELD = 2000, PING = 25_000;
const GRACE = 20_000;            // a closed tab's ant waits this long for a reload
const LIMITS = { ants: 400, perIP: 8, greetEvery: 10_000, greetsPerIPMinute: 12, hello: 5_000, frame: 512 };
if (!KEY) { console.error('presence: TICKET_KEY is not set'); process.exit(1); }

const chart = readFileSync(new URL('./visitor.scxml', import.meta.url), 'utf8');
const clients = new Map();       // handle -> client
const byTab = new Map();         // tab id -> client, while connected
const leaving = new Map();       // handle -> timer, while its tab may come back
const perIP = new Map();         // ip -> open sockets
const greetsByIP = new Map();    // ip -> [times]
const nonces = new Map();        // nonce -> exp: each ticket is good once
let broadcast = [];
const started = Date.now();

const world = createWorld(tinyactors, {
  chart,
  emit(to, message) {
    if (to === null) broadcast.push(message);
    else clients.get(to)?.out.push(message);
  },
  config: { system: { maxActors: LIMITS.ants + 64 } },
});

// ---- the tick: the world's arrivals, then one frame per client
setInterval(async () => {
  try {
    await world.tick();
  } catch (error) {
    console.error('presence: tick', error);
  }
  const shared = broadcast;
  broadcast = [];
  for (const client of clients.values()) {
    const messages = shared.length ? (client.out.length ? [...shared, ...client.out] : shared) : client.out;
    client.out = [];
    if (messages.length && client.socket.readyState === 1) client.socket.send(JSON.stringify({ t: 'batch', m: messages }));
  }
  const now = Date.now();
  for (const [nonce, exp] of nonces) if (exp < now) nonces.delete(nonce);
}, TICK);

setInterval(() => {
  for (const client of clients.values()) {
    if (client.socket.readyState === 1) client.socket.send(JSON.stringify({ t: 'field', ...world.field(client.country) }));
  }
}, FIELD);

// ---- HTTP: the health check; everything else is the WebSocket
const server = http.createServer((request, response) => {
  if (request.url === '/health') {
    response.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    response.end(JSON.stringify({ ok: true, uptime: Math.round((Date.now() - started) / 1000), connected: clients.size, ...world.stats() }));
    return;
  }
  response.writeHead(426, { 'content-type': 'text/plain' });
  response.end('presence: a WebSocket endpoint (website/docs/presence.md)\n');
});

const sockets = new WebSocketServer({ noServer: true, maxPayload: LIMITS.frame });
server.on('upgrade', (request, socket, head) => {
  const ip = String(request.headers['x-forwarded-for'] ?? request.socket.remoteAddress ?? '').split(',')[0].trim();
  if (!ORIGINS.has(request.headers.origin)) return refuse(socket, 403);
  if ((perIP.get(ip) ?? 0) >= LIMITS.perIP) return refuse(socket, 429);
  sockets.handleUpgrade(request, socket, head, (ws) => connected(ws, ip));
});
function refuse(socket, status) {
  socket.end(`HTTP/1.1 ${status} ${http.STATUS_CODES[status]}\r\nconnection: close\r\n\r\n`);
}

function connected(socket, ip) {
  perIP.set(ip, (perIP.get(ip) ?? 0) + 1);
  let client = null;
  socket.alive = true;
  socket.on('pong', () => { socket.alive = true; });
  const helloTimer = setTimeout(() => socket.close(4008, 'no hello'), LIMITS.hello);

  socket.on('message', async (raw) => {
    let message;
    try { message = JSON.parse(String(raw)); } catch { return socket.close(4000, 'not JSON'); }
    if (!client) {
      if (message.t !== 'hello') return socket.close(4000, 'hello first');
      clearTimeout(helloTimer);
      client = await hello(socket, ip, message);
      return;
    }
    if (message.t === 'greet') return greet(client, Number(message.h));
    if (message.t === 'hide') return world.hide(client.handle);
    if (message.t === 'show') return world.show(client.handle);
    if (message.t === 'leave') { client.left = true; world.leave(client.handle); return socket.close(1000, 'left'); }
  });

  socket.on('close', () => {
    clearTimeout(helloTimer);
    perIP.set(ip, (perIP.get(ip) ?? 1) - 1);
    if (perIP.get(ip) <= 0) perIP.delete(ip);
    if (!client || clients.get(client.handle) !== client) return;
    clients.delete(client.handle);
    byTab.delete(client.tab);
    if (client.left) return;
    // The tab may be reloading: its ant waits, asleep, for GRACE.
    world.hide(client.handle);
    leaving.set(client.handle, setTimeout(() => { leaving.delete(client.handle); world.leave(client.handle); }, GRACE));
  });
}

async function hello(socket, ip, { ticket, tab }) {
  const claims = await check(KEY, ticket);
  if (!claims || nonces.has(claims.nonce)) { socket.close(4001, 'ticket'); return null; }
  nonces.set(claims.nonce, claims.exp);
  if (typeof tab !== 'string' || !/^[A-Za-z0-9_-]{8,64}$/.test(tab)) { socket.close(4000, 'tab'); return null; }
  if (byTab.has(tab)) { socket.close(4009, 'tab already connected'); return null; }
  let handle = world.find(tab);
  if (handle !== undefined) {
    clearTimeout(leaving.get(handle));
    leaving.delete(handle);
    world.show(handle);
  } else {
    if (world.stats().ants >= LIMITS.ants) { socket.close(4029, 'full'); return null; }
    handle = world.join(tab, claims.country);
  }
  const client = { socket, ip, tab, handle, country: claims.country, out: [], lastGreet: 0, left: false };
  clients.set(handle, client);
  byTab.set(tab, client);
  socket.send(JSON.stringify({ t: 'welcome', you: handle, now: world.host.now(), ants: world.snapshot(), field: world.field(claims.country) }));
  return client;
}

function greet(client, target) {
  const now = Date.now();
  if (now - client.lastGreet < LIMITS.greetEvery) return;
  const times = (greetsByIP.get(client.ip) ?? []).filter((t) => now - t < 60_000);
  if (times.length >= LIMITS.greetsPerIPMinute) return;
  times.push(now);
  greetsByIP.set(client.ip, times);
  client.lastGreet = now;
  world.greet(client.handle, target);
}

// ---- keepalive: exe's proxy and Cloudflare's close idle connections
setInterval(() => {
  for (const socket of sockets.clients) {
    if (!socket.alive) { socket.terminate(); continue; }
    socket.alive = false;
    socket.ping();
  }
}, PING);

server.listen(PORT, () => console.log(`presence: listening on ${server.address().port}, origins ${[...ORIGINS].join(' ')}`));
for (const signal of ['SIGTERM', 'SIGINT']) {
  process.on(signal, () => {
    for (const socket of sockets.clients) socket.close(1012, 'restarting');
    server.close();
    world.close();
    process.exit(0);
  });
}
