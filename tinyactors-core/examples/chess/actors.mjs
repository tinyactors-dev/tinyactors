/**
 * The chess server's actors, as charts (give them the API of either binding
 * and the system to start games in).
 *
 *   lobby      systemID "lobby". CREATE {name} opens a game with a fresh join
 *              code; JOIN {code, name} routes to it (or answers
 *              NO_SUCH_GAME); LIST answers GAMES. Games report back
 *              (GAME_OPENED / GAME_STARTED / GAME_OVER / GAME_GONE).
 *   game       systemID "game:<CODE>". Two players (the addresses of their
 *              actors in the browsers, reached over the link), any number of
 *              spectators. MOVE {from, to, promotion} is checked against the
 *              rules and the player's colour; every change is broadcast as
 *              STATE. Idle games expire. A finished game is reported to
 *              `options.records` (an ActorRef — the records actor in
 *              PostgreSQL, records.scxml) as game.over.
 *
 * A player is whoever sent the event (_event.origin): the server answers it
 * through the link without knowing it is one. The lobby passes the player
 * on when it forwards a JOIN.
 */
import { START, fromFEN, toFEN, play, status } from './chess.mjs';

const CODE_CHARS = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789'; /* no 0/O, 1/I/L */
const OVER = ['checkmate', 'stalemate', 'draw'];

export function chessActors({ scxml, createActor }, system, options = {}) {
  const idleMs = options.idleMs || 30 * 60 * 1000;
  const waitMs = options.waitMs || 10 * 60 * 1000;
  const keepMs = options.keepMs || 10 * 60 * 1000;
  const random = options.random || Math.random;
  const c = scxml.chart();

  const colorOf = (players, player) => (players.w === player ? 'w' : players.b === player ? 'b' : null);
  const view = (s, you) => ({
    type: 'STATE',
    code: s.code,
    fen: toFEN(s.pos),
    status: s.status,
    names: s.names,
    lastMove: s.lastMove,
    moves: s.moves.length,
    spectators: s.spectators.length,
    you,
  });
  /* host code may send to any number of actors (a <send> addresses one) */
  const broadcast = c.script(c.expr((s) => {
    for (const p of [s.players.w, s.players.b, ...s.spectators].filter(Boolean)) system.send(p, view(s, colorOf(s.players, p) || 'spectator'));
  }));
  /* a finished game goes on record (in PostgreSQL, when the server has it) */
  const report = c.script(c.expr((s) => {
    if (!options.records || !s.players.b) return;
    options.records.send({ type: 'game.over', code: s.code, names: s.names, status: s.status, moves: s.moves.length, players: s.players });
  }));
  /* a <send> of an event object to an address computed from the session */
  const tell = (to, event) =>
    c.send(undefined, { eventexpr: c.expr((s) => event(s).type), targetexpr: c.expr(to) }, c.content(c.expr(event)));
  const player = ({ _event }) => _event.data.player || _event.origin;
  const self = ({ _sessionid }) => `tinyactors://${system.id}/#_scxml_${_sessionid}`;
  const timer = (name, ms) => [c.onentry(c.send(name, { delay: `${ms}ms`, id: name })), c.onexit(c.cancel(name))];
  const toLobby = (event) => tell(({ lobby }) => lobby, event);
  const joined = (color) => tell(player, (s) => ({ type: 'JOINED', code: s.code, color: color(s) }));
  const spectate = [
    c.assign('spectators', c.expr((s) => [...s.spectators, player(s)])),
    tell(player, (s) => ({ type: 'SPECTATING', code: s.code })),
    broadcast,
  ];

  const game = c.scxml('game', { initial: 'waiting', datamodel: 'ecmascript' },
    /* input: code, lobby, host (an address), hostName */
    c.datamodel({
      code: null,
      lobby: null,
      host: null,
      hostName: 'White',
      pos: c.expr(() => fromFEN(START)),
      players: c.expr(({ host }) => ({ w: host, b: null })),
      names: c.expr(({ hostName }) => ({ w: hostName, b: null })),
      spectators: [],
      moves: [],
      lastMove: null,
      status: c.expr(() => ({ state: 'waiting' })),
    }),
    c.state('waiting',
      c.onentry(
        tell(({ players }) => players.w, (s) => ({ type: 'JOINED', code: s.code, color: 'w' })),
        toLobby((s) => ({ type: 'GAME_OPENED', code: s.code, host: s.names.w }))),
      ...timer('wait', waitMs),
      c.transition('JOIN', undefined, { cond: c.expr((s) => s.players.w === player(s)) }, joined(() => 'w')),
      c.transition('JOIN', 'playing',
        c.assign('players', c.expr((s) => ({ ...s.players, b: player(s) }))),
        c.assign('names', c.expr((s) => ({ ...s.names, b: s._event.data.name || 'Black' }))),
        c.assign('status', c.expr(() => ({ state: 'playing' }))),
        joined(() => 'b'),
        toLobby((s) => ({ type: 'GAME_STARTED', code: s.code, players: s.names }))),
      c.transition('wait', 'abandoned')),
    c.state('playing',
      c.onentry(broadcast),
      ...timer('idle', idleMs),
      /* a legal move re-enters the state: the idle timer restarts, everyone hears */
      c.transition('MOVE', 'playing', { cond: c.expr((s) => colorOf(s.players, player(s)) === s.pos.turn && play(s.pos, s._event.data) !== null) },
        c.assign('pos', c.expr((s) => play(s.pos, s._event.data))),
        c.assign('moves', c.expr(({ moves, _event: { data } }) => [...moves, `${data.from}${data.to}${data.promotion || ''}`])),
        c.assign('lastMove', c.expr(({ _event: { data } }) => ({ from: data.from, to: data.to }))),
        c.assign('status', c.expr((s) => status(s.pos)))),
      c.transition('MOVE', undefined,
        tell(player, (s) => ({
          type: 'REJECTED',
          move: { from: s._event.data.from, to: s._event.data.to },
          reason: colorOf(s.players, player(s)) !== s.pos.turn ? 'not your turn' : 'illegal move',
        }))),
      c.transition('RESIGN', 'over', { cond: c.expr((s) => colorOf(s.players, player(s)) !== null) },
        c.assign('status', c.expr((s) => ({ state: 'resigned', winner: colorOf(s.players, player(s)) === 'w' ? 'b' : 'w' })))),
      /* a player reconnecting, or a spectator */
      c.transition('JOIN', undefined, { cond: c.expr((s) => colorOf(s.players, player(s)) !== null) }, joined((s) => colorOf(s.players, player(s))), broadcast),
      c.transition('JOIN', undefined, ...spectate),
      c.transition(undefined, 'over', { cond: c.expr((s) => OVER.includes(s.status.state)) }),
      c.transition('idle', 'over', c.assign('status', c.expr(() => ({ state: 'abandoned' }))))),
    c.state('over',
      c.onentry(broadcast, report, toLobby((s) => ({ type: 'GAME_OVER', code: s.code, status: s.status }))),
      ...timer('keep', keepMs),
      c.transition('JOIN', undefined, ...spectate),
      c.transition('keep', 'gone')),
    c.state('abandoned', c.transition(undefined, 'gone')),
    c.final('gone', c.onentry(toLobby((s) => ({ type: 'GAME_GONE', code: s.code })))));

  const newCode = (games) => {
    for (;;) {
      let code = '';
      for (let i = 0; i < 5; i++) code += CODE_CHARS[Math.floor(random() * CODE_CHARS.length)];
      if (!games[code]) return code;
    }
  };
  const codeOf = ({ _event }) => String(_event.data.code || '').trim().toUpperCase();
  const record = (update) => c.assign('games', c.expr((s) => ({ ...s.games, [s._event.data.code]: { ...s.games[s._event.data.code], ...update(s) } })));

  const lobby = c.scxml('lobby', { initial: 'open', datamodel: 'ecmascript' },
    c.datamodel({ games: c.expr(() => ({})) }),
    c.state('open',
      /* a game is an actor of its own, found by its code */
      c.transition('CREATE', undefined, c.script(c.expr((s) => {
        const code = newCode(s.games);
        s.games[code] = { code, state: 'starting' };
        createActor(game, { system, systemID: `game:${code}`, input: { code, lobby: self(s), host: s._event.origin, hostName: s._event.data.name || 'White' } }).start();
      }))),
      c.transition('JOIN', undefined, { cond: c.expr((s) => !!(s.games[codeOf(s)] && s.games[codeOf(s)].address)) },
        tell((s) => s.games[codeOf(s)].address, (s) => ({ type: 'JOIN', player: s._event.origin, name: s._event.data.name }))),
      c.transition('JOIN', undefined, tell(({ _event }) => _event.origin, (s) => ({ type: 'NO_SUCH_GAME', code: codeOf(s) }))),
      c.transition('LIST', undefined,
        tell(({ _event }) => _event.origin, (s) => ({ type: 'GAMES', games: Object.values(s.games).filter((g) => g.state === 'waiting') }))),
      c.transition('GAME_OPENED', undefined, record(({ _event }) => ({ code: _event.data.code, host: _event.data.host, state: 'waiting', address: _event.origin }))),
      c.transition('GAME_STARTED', undefined, record(({ _event }) => ({ players: _event.data.players, state: 'playing' }))),
      c.transition('GAME_OVER', undefined, record(({ _event }) => ({ state: 'over', result: _event.data.status }))),
      c.transition('GAME_GONE', undefined,
        c.assign('games', c.expr(({ games, _event }) => Object.fromEntries(Object.entries(games).filter(([code]) => code !== _event.data.code)))))));

  return { lobby, game };
}
