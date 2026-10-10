// End to end with PostgreSQL: two "browsers" (web binding) play over real
// WebSockets against the chess server (Node binding); the finished game goes
// to the records actor in PostgreSQL (store link), which sends each player
// their standing straight back to the browser: browser -> node -> PostgreSQL
// -> browser. Skipped when PostgreSQL is not running (mise run pg:start).
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import * as web from '../../../bindings/web/node.mjs';
import { chessActors } from '../actors.mjs';
import { playerChart, screenOf } from '../client/player.mjs';
import { connectRecords } from '../records.mjs';
import { available, connect } from '../../../tools/pgwire.mjs';

const server = createRequire(import.meta.url)('../../../bindings/node');
const PORT = Number(process.env.TA_PG_PORT || 54320);
const DB = 'tinyactors_chess_e2e';

const until = async (cond, what, ms = 6000) => {
  const t0 = Date.now();
  while (!(await cond())) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 10));
  }
};

test('postgres: finished games are kept in PostgreSQL; standings come back to the browsers', async (t) => {
  if (!(await available({ port: PORT }))) return t.skip('PostgreSQL is not running (mise run pg:start)');
  const admin = await connect({ port: PORT, database: 'postgres' });
  await admin.query(`DROP DATABASE IF EXISTS ${DB} WITH (FORCE)`);
  await admin.query(`CREATE DATABASE ${DB}`);
  await admin.query(`ALTER DATABASE ${DB} SET tinyactors.system_id = 'chess-db'`);
  await admin.end();

  const sys = server.createSystem({ systemID: 'chess' });
  const records = await connectRecords(server, sys, { port: PORT, database: DB });
  assert.equal(records.link.id, 'chess-db');
  const { lobby } = chessActors(server, sys, { records: records.ref });
  server.createActor(lobby, { system: sys, systemID: 'lobby' }).start();
  const served = server.serveLinks(sys, { port: 0, exports: { '*': 'CREATE JOIN MOVE RESIGN' }, announce: [records.link.id] });
  await until(() => served.port, 'the server');
  const url = `ws://127.0.0.1:${served.port}/link`;

  const browser = (name) => {
    const system = web.createSystem({ systemID: name });
    const link = web.createLink(system, { url, name: 'chess' });
    const player = web.createActor(playerChart(web), { system, input: { lobby: link.get('lobby').address, name } }).start();
    return { system, link, player, ctx: () => player.getSnapshot().context, value: () => screenOf(player.getSnapshot().value) };
  };
  const play = async (white, black) => {
    white.player.send({ type: 'CREATE' });
    await until(() => white.value() === 'waiting', 'a join code');
    black.player.send({ type: 'JOIN', code: white.ctx().code });
    await until(() => white.value() === 'playing' && black.value() === 'playing' && black.ctx().fen, 'the game to start');
    /* fool's mate: black wins */
    for (const [who, from, to] of [[white, 'f2', 'f3'], [black, 'e7', 'e5'], [white, 'g2', 'g4'], [black, 'd8', 'h4']]) {
      const n = white.ctx().moves;
      who.player.send({ type: 'MOVE', from, to });
      await until(() => white.ctx().moves === n + 1 && black.ctx().moves === n + 1, `${from}${to}`);
    }
    await until(() => white.value() === 'over' && black.value() === 'over', 'the game to end');
  };

  const ann = browser('ann');
  const ben = browser('ben');
  await play(ann, ben);
  // the records actor in PostgreSQL answered each browser directly
  await until(() => ann.ctx().record && ben.ctx().record, 'RECORD from PostgreSQL');
  assert.deepEqual(ann.ctx().record, { name: 'ann', wins: 0, losses: 1, draws: 0, games: 1 });
  assert.deepEqual(ben.ctx().record, { name: 'ben', wins: 1, losses: 0, draws: 0, games: 1 });

  // a rematch, colours swapped: the standings add up in the database
  ann.player.send({ type: 'NEW_GAME' });
  ben.player.send({ type: 'NEW_GAME' });
  await until(() => ann.value() === 'lobby' && ben.value() === 'lobby', 'the lobby');
  await play(ben, ann);
  await until(() => ann.ctx().record?.games === 2 && ben.ctx().record?.games === 2, 'the second RECORD');
  assert.deepEqual(ann.ctx().record, { name: 'ann', wins: 1, losses: 1, draws: 0, games: 2 });
  assert.deepEqual(await records.standing('ben'), { wins: 1, losses: 1, draws: 0, games: 2 });
  const games = await records.games();
  assert.equal(games.length, 2);
  assert.deepEqual(games[0].names, { w: 'ann', b: 'ben' });
  assert.deepEqual(games[0].status, { state: 'checkmate', winner: 'b' });

  for (const b of [ann, ben]) b.link.close();
  await served.close();
  for (const b of [ann, ben]) b.system.stop();
  await records.close();
  sys.stop();
});
