#!/usr/bin/env node
/**
 * The chess server: a lobby actor and one actor per game (charts, actors.mjs),
 * in one tinyactors system (Node.js N-API binding). Browsers link in over a WebSocket (/link)
 * and talk to these actors as if they were local. The same HTTP server serves
 * the client and the web binding.
 *
 *   mise run example:chess [--port 8080] [--no-pg]
 *   (or: mise run node:build ::: web:build, then node examples/chess/server.mjs)
 *
 * With PostgreSQL running (mise run pg:start; TA_PG_PORT, CHESS_DB), finished
 * games and players' standings are kept by an actor in the database
 * (records.scxml), reached through a store link: game (here) -> records
 * (PostgreSQL) -> RECORD straight to the players in their browsers.
 */
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chessActors } from './actors.mjs';
import { available } from '../../tools/pgwire.mjs';
import { connectRecords } from './records.mjs';

const ta = createRequire(import.meta.url)('../../bindings/node');
const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const arg = process.argv.indexOf('--port');
const PORT = Number(arg > 0 ? process.argv[arg + 1] : process.env.PORT || 8080);

/* idle games are unloaded from memory every minute and come back when someone moves or joins */
const system = ta.createSystem({ systemID: 'chess', hibernation: { store: new Map() } });
setInterval(() => system.sweep(), 60_000).unref();
const PG_PORT = Number(process.env.TA_PG_PORT || 54320);
const records =
  !process.argv.includes('--no-pg') && (await available({ port: PG_PORT }))
    ? await connectRecords(ta, system, { port: PG_PORT, database: process.env.CHESS_DB || 'tinyactors' })
    : null;
if (records) console.log(`chess: game records in PostgreSQL (tinyactors://${records.link.id}/@records)`);
else console.log(process.argv.includes('--no-pg') ? 'chess: no game records (--no-pg)' : `chess: no PostgreSQL on port ${PG_PORT}, no game records (mise run pg:start keeps them)`);
const { lobby } = chessActors(ta, system, { records: records && records.ref });
const lobbyActor = ta.createActor(lobby, { system, systemID: 'lobby' }).start();

const MIME = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.svg': 'image/svg+xml', '.json': 'application/json' };
const PUBLIC = ['/examples/chess/', '/bindings/web/'];

const server = http.createServer(async (req, res) => {
  let path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (path === '/') {
    res.writeHead(302, { location: '/examples/chess/client/' });
    return res.end();
  }
  if (path === '/stats') {
    const games = Object.values(lobbyActor.getSnapshot().context.games);
    res.writeHead(200, { 'content-type': 'application/json' });
    return res.end(JSON.stringify({ system: system.stats(), links: links.connections().length, games }, null, 2));
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
});

/* browsers are untrusted peers: they may send these events to the lobby and
 * to games (addressed by the refs the server handed out), nothing else */
const links = ta.serveLinks(system, {
  server,
  path: '/link',
  exports: { '*': 'CREATE JOIN MOVE RESIGN' },
  announce: records ? [records.link.id] : false, /* browsers accept RECORDs from the database */
});
server.listen(PORT, () => console.log(`chess: http://localhost:${server.address().port}/  (links on /link, stats on /stats)`));
