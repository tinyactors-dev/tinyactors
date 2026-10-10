// The whole thing over real WebSockets: the server's lobby and games run on
// the Node.js (N-API) binding, the "browsers" on the web (wasm) binding.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import * as web from '../../../bindings/web/node.mjs';
import { chessActors } from '../actors.mjs';
import { playerChart, screenOf } from '../client/player.mjs';

const server = createRequire(import.meta.url)('../../../bindings/node');

const until = async (cond, what, ms = 4000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 5));
  }
};

test('lobby, join by code, a game across the link, spectators, rejections, hibernation', async () => {
  const sys = server.createSystem({ systemID: 'chess', hibernation: { store: new Map() } });
  const { lobby } = chessActors(server, sys);
  const lobbyActor = server.createActor(lobby, { system: sys, systemID: 'lobby' }).start();
  const served = server.serveLinks(sys, { port: 0, exports: { '*': 'CREATE JOIN MOVE RESIGN' } });
  await until(() => served.port, 'the server');
  const url = `ws://127.0.0.1:${served.port}/link`;

  const browser = (name) => {
    const system = web.createSystem({ systemID: name });
    const link = web.createLink(system, { url, name: 'chess' });
    const player = web.createActor(playerChart(web), { system, input: { lobby: link.get('lobby').address, name } }).start();
    return { system, link, player, ctx: () => player.getSnapshot().context, value: () => screenOf(player.getSnapshot().value) };
  };
  const alice = browser('alice');
  const bob = browser('bob');
  const carol = browser('carol');

  alice.player.send({ type: 'CREATE' });
  await until(() => alice.value() === 'waiting', 'a join code');
  const code = alice.ctx().code;
  assert.match(code, /^[A-Z2-9]{5}$/);
  assert.equal(alice.link.status().state, 'open');

  bob.player.send({ type: 'JOIN', code: 'QQQQQ' });
  await until(() => bob.value() === 'lobby' && bob.ctx().error, 'NO_SUCH_GAME');
  assert.equal(bob.ctx().error, 'no game with that code');

  bob.player.send({ type: 'JOIN', code: code.toLowerCase() });
  await until(() => alice.value() === 'playing' && bob.value() === 'playing' && bob.ctx().fen, 'the game to start');
  assert.equal(alice.ctx().color, 'w');
  assert.equal(bob.ctx().color, 'b');
  assert.deepEqual(alice.ctx().names, { w: 'alice', b: 'bob' });

  bob.player.send({ type: 'MOVE', from: 'e7', to: 'e5' });
  await until(() => bob.ctx().error, 'a rejection');
  assert.match(bob.ctx().error, /not your turn/);
  alice.player.send({ type: 'MOVE', from: 'e2', to: 'e5' });
  await until(() => alice.ctx().error, 'a rejection');
  assert.match(alice.ctx().error, /illegal move/);

  carol.player.send({ type: 'JOIN', code });
  await until(() => carol.value() === 'watching' && carol.ctx().fen, 'a spectator');

  const moves = [[alice, 'f2', 'f3'], [bob, 'e7', 'e5'], [alice, 'g2', 'g4'], [bob, 'd8', 'h4']];
  for (const [who, from, to] of moves) {
    const n = alice.ctx().moves;
    who.player.send({ type: 'MOVE', from, to });
    await until(() => alice.ctx().moves === n + 1 && bob.ctx().moves === n + 1 && carol.ctx().moves === n + 1, `${from}${to}`);
  }
  await until(() => alice.value() === 'over' && bob.value() === 'over', 'the game to end');
  assert.deepEqual(bob.ctx().status, { state: 'checkmate', winner: 'b' });
  assert.deepEqual(carol.ctx().status, { state: 'checkmate', winner: 'b' });
  assert.deepEqual(alice.ctx().lastMove, { from: 'd8', to: 'h4' });
  await until(() => lobbyActor.getSnapshot().context.games[code]?.state === 'over', 'the lobby to record it');

  // the finished game is idle: the server hibernates it; a late spectator wakes it
  sys.sweep();
  assert.ok(sys.sweep() >= 1, 'idle actors hibernated');
  const dave = browser('dave');
  dave.player.send({ type: 'JOIN', code });
  await until(() => dave.value() === 'watching' && dave.ctx().status?.state === 'checkmate', 'a woken game');
  assert.equal(sys.stats().hibernated, 0, 'woken');

  for (const b of [alice, bob, carol, dave]) b.link.close();
  await served.close();
  for (const b of [alice, bob, carol, dave]) b.system.stop();
  sys.stop(); /* games keep timers (idle expiry): stop ends them */
});
