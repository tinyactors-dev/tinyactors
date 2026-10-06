/**
 * The chess server's actors (pure machine definitions; give them the API of
 * either binding and the system to start games in).
 *
 *   lobby      systemID "lobby". CREATE {player, name} opens a game with a
 *              fresh join code; JOIN {code, player, name} routes to it (or
 *              answers NO_SUCH_GAME); LIST answers GAMES. Games report back
 *              (GAME_OPENED / GAME_STARTED / GAME_OVER / GAME_GONE).
 *   game       systemID "game:<CODE>". Two players (refs to their actors in
 *              the browsers, reached over the link), any number of
 *              spectators. MOVE {from, to, promotion, player} is checked
 *              against the rules and the player's colour; every change is
 *              broadcast as STATE. Idle games expire. A finished game is
 *              reported to `options.records` (an ActorRef — the records actor
 *              in PostgreSQL, records.scxml) as game.over.
 *
 * Players are ActorRefs carried in events (`player: self` on the client) —
 * the server answers them through the link without knowing it is one.
 */
import { START, fromFEN, toFEN, play, status } from './chess.mjs';

const CODE_CHARS = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789'; /* no 0/O, 1/I/L */

export function chessActors({ createMachine, createActor, assign, sendTo }, system, options = {}) {
  const idleMs = options.idleMs || 30 * 60 * 1000;
  const waitMs = options.waitMs || 10 * 60 * 1000;
  const keepMs = options.keepMs || 10 * 60 * 1000;
  const random = options.random || Math.random;

  const same = (a, b) => !!a && !!b && a.address === b.address;
  const colorOf = (context, player) => (same(context.players.w, player) ? 'w' : same(context.players.b, player) ? 'b' : null);
  const view = (context, extra = {}) => ({
    type: 'STATE',
    code: context.code,
    fen: toFEN(context.pos),
    status: context.status,
    names: context.names,
    lastMove: context.lastMove,
    moves: context.moves.length,
    spectators: context.spectators.length,
    ...extra,
  });
  const everyone = (context) => [context.players.w, context.players.b, ...context.spectators].filter(Boolean);
  /* a finished game goes on record (in PostgreSQL, when the server has it) */
  const report = ({ context }) => {
    if (!options.records || !context.players.b) return;
    const { w, b } = context.players;
    options.records.send({ type: 'game.over', code: context.code, names: context.names, status: context.status, moves: context.moves.length, players: { w, b } });
  };
  /* host code may send to any number of refs (sendTo addresses one) */
  const broadcast = ({ context }) => {
    for (const p of everyone(context)) p.send(view(context, { you: colorOf(context, p) || 'spectator' }));
  };

  const game = createMachine(
    {
      id: 'game',
      context: ({ input }) => ({
        code: input.code,
        lobby: input.lobby,
        pos: fromFEN(START),
        players: { w: input.host, b: null },
        names: { w: input.hostName, b: null },
        spectators: [],
        moves: [],
        lastMove: null,
        status: { state: 'waiting' },
      }),
      initial: 'waiting',
      states: {
        waiting: {
          entry: [
            sendTo(({ context }) => context.players.w, ({ context, self }) => ({ type: 'JOINED', code: context.code, color: 'w', game: self })),
            sendTo(({ context }) => context.lobby, ({ context }) => ({ type: 'GAME_OPENED', code: context.code, host: context.names.w })),
          ],
          on: {
            JOIN: [
              {
                guard: ({ context, event }) => same(context.players.w, event.player),
                actions: sendTo(({ event }) => event.player, ({ context, self }) => ({ type: 'JOINED', code: context.code, color: 'w', game: self })),
              },
              {
                target: 'playing',
                actions: [
                  assign(({ context, event }) => ({
                    players: { ...context.players, b: event.player },
                    names: { ...context.names, b: event.name || 'Black' },
                    status: { state: 'playing' },
                  })),
                  sendTo(({ event }) => event.player, ({ context, self }) => ({ type: 'JOINED', code: context.code, color: 'b', game: self })),
                  sendTo(({ context }) => context.lobby, ({ context }) => ({ type: 'GAME_STARTED', code: context.code, players: context.names })),
                ],
              },
            ],
          },
          after: { wait: 'abandoned' },
        },
        playing: {
          entry: broadcast,
          on: {
            MOVE: [
              {
                guard: 'legal',
                target: 'playing',
                reenter: true, /* restarts the idle timer, broadcasts */
                actions: [
                  assign({
                    pos: ({ context, event }) => play(context.pos, event),
                    moves: ({ context, event }) => [...context.moves, `${event.from}${event.to}${event.promotion || ''}`],
                    lastMove: ({ event }) => ({ from: event.from, to: event.to }),
                  }),
                  assign({ status: ({ context }) => status(context.pos) }),
                ],
              },
              {
                actions: sendTo(
                  ({ event }) => event.player,
                  ({ context, event }) => ({
                    type: 'REJECTED',
                    move: { from: event.from, to: event.to },
                    reason: colorOf(context, event.player) !== context.pos.turn ? 'not your turn' : 'illegal move',
                  }),
                ),
              },
            ],
            RESIGN: {
              guard: ({ context, event }) => colorOf(context, event.player) !== null,
              target: 'over',
              actions: assign(({ context, event }) => ({
                status: { state: 'resigned', winner: colorOf(context, event.player) === 'w' ? 'b' : 'w' },
              })),
            },
            JOIN: [
              {
                guard: ({ context, event }) => colorOf(context, event.player) !== null, /* a player reconnecting */
                actions: [
                  sendTo(({ event }) => event.player, ({ context, event, self }) => ({ type: 'JOINED', code: context.code, color: colorOf(context, event.player), game: self })),
                  broadcast,
                ],
              },
              {
                actions: [
                  assign(({ context, event }) => ({ spectators: [...context.spectators, event.player] })),
                  sendTo(({ event }) => event.player, ({ context, self }) => ({ type: 'SPECTATING', code: context.code, game: self })),
                  broadcast,
                ],
              },
            ],
          },
          always: { guard: ({ context }) => ['checkmate', 'stalemate', 'draw'].includes(context.status.state), target: 'over' },
          after: { idle: { target: 'over', actions: assign({ status: { state: 'abandoned' } }) } },
        },
        over: {
          entry: [broadcast, report, sendTo(({ context }) => context.lobby, ({ context }) => ({ type: 'GAME_OVER', code: context.code, status: context.status }))],
          on: {
            JOIN: {
              actions: [
                assign(({ context, event }) => ({ spectators: [...context.spectators, event.player] })),
                sendTo(({ event }) => event.player, ({ context, self }) => ({ type: 'SPECTATING', code: context.code, game: self })),
                broadcast,
              ],
            },
          },
          after: { keep: 'gone' },
        },
        abandoned: { always: 'gone' },
        gone: { type: 'final', entry: sendTo(({ context }) => context.lobby, ({ context }) => ({ type: 'GAME_GONE', code: context.code })) },
      },
    },
    {
      guards: {
        legal: ({ context, event }) => colorOf(context, event.player) === context.pos.turn && play(context.pos, event) !== null,
      },
      delays: { wait: waitMs, idle: idleMs, keep: keepMs },
    },
  );

  const newCode = () => {
    for (;;) {
      let c = '';
      for (let i = 0; i < 5; i++) c += CODE_CHARS[Math.floor(random() * CODE_CHARS.length)];
      if (!system.get(`game:${c}`)) return c;
    }
  };

  const lobby = createMachine(
    {
      id: 'lobby',
      context: { games: {} },
      initial: 'open',
      states: {
        open: {
          on: {
            CREATE: { actions: 'openGame' },
            JOIN: [
              {
                guard: ({ context, event }) => !!context.games[String(event.code || '').toUpperCase()],
                actions: sendTo(
                  ({ system: s, event }) => s.get(`game:${String(event.code).toUpperCase()}`),
                  ({ event }) => ({ type: 'JOIN', player: event.player, name: event.name }),
                ),
              },
              { actions: sendTo(({ event }) => event.player, ({ event }) => ({ type: 'NO_SUCH_GAME', code: event.code })) },
            ],
            LIST: {
              actions: sendTo(
                ({ event }) => event.player,
                ({ context }) => ({ type: 'GAMES', games: Object.values(context.games).filter((g) => g.state === 'waiting') }),
              ),
            },
            GAME_OPENED: { actions: assign(({ context, event }) => ({ games: { ...context.games, [event.code]: { code: event.code, host: event.host, state: 'waiting' } } })) },
            GAME_STARTED: { actions: assign(({ context, event }) => ({ games: { ...context.games, [event.code]: { ...context.games[event.code], players: event.players, state: 'playing' } } })) },
            GAME_OVER: { actions: assign(({ context, event }) => ({ games: { ...context.games, [event.code]: { ...context.games[event.code], state: 'over', result: event.status } } })) },
            GAME_GONE: {
              actions: assign({
                games: ({ context, event }) => Object.fromEntries(Object.entries(context.games).filter(([code]) => code !== event.code)),
              }),
            },
          },
        },
      },
    },
    {
      actions: {
        /* a game is an actor of its own, found by its code */
        openGame: ({ event, self }) => {
          const code = newCode();
          createActor(game, { system, systemID: `game:${code}`, input: { code, lobby: self, host: event.player, hostName: event.name || 'White' } }).start();
        },
      },
    },
  );

  return { lobby, game };
}
