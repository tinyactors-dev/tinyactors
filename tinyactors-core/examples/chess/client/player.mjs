/**
 * The browser side: one actor per player, talking to the server's lobby and
 * game actors through a link (it does not know they are remote).
 *
 *   lobby → creating → waiting (show the code) → playing → over
 *         ↘ joining ↗ (JOINED)  ↘ watching            ↺ NEW_GAME
 *
 * Rejoining (after a reload) is a JOIN with the same address: the game
 * recognizes the player and resends JOINED + STATE.
 *
 * UI events: CREATE {name}, JOIN {code, name}, MOVE {from, to, promotion},
 * RESIGN, NEW_GAME. Server events: JOINED, SPECTATING, NO_SUCH_GAME, STATE,
 * REJECTED; and RECORD {name, wins, losses, draws, games} — from the records
 * actor in PostgreSQL (an SCXML document: its <param>s are event.data).
 */
export function playerMachine({ createMachine, assign, sendTo }) {
  const fromState = assign(({ event }) => ({
    fen: event.fen,
    status: event.status,
    names: event.names,
    lastMove: event.lastMove,
    moves: event.moves,
    spectators: event.spectators,
    color: event.you === 'spectator' ? null : event.you,
    error: null,
  }));
  const ended = ({ context }) => !!context.status && !['waiting', 'playing', 'check'].includes(context.status.state);
  const fail = (message) => assign({ error: message });

  return createMachine({
    id: 'player',
    context: ({ input }) => ({
      lobby: input.lobby,
      name: input.name || 'Player',
      code: null,
      game: null, /* the server's game actor (an ActorRef) */
      color: null,
      fen: null,
      status: null,
      names: null,
      lastMove: null,
      moves: 0,
      spectators: 0,
      error: null,
      record: null, /* this player's standing, kept in PostgreSQL */
    }),
    on: { RECORD: { actions: assign({ record: ({ event }) => event.data }) } },
    initial: 'lobby',
    states: {
      lobby: {
        on: {
          CREATE: {
            target: 'creating',
            actions: [
              assign(({ event, context }) => ({ name: event.name || context.name, error: null })),
              sendTo(({ context }) => context.lobby, ({ self, context }) => ({ type: 'CREATE', player: self, name: context.name })),
            ],
          },
          JOIN: {
            target: 'joining',
            actions: [
              assign(({ event, context }) => ({ name: event.name || context.name, code: String(event.code).trim().toUpperCase(), error: null })),
              sendTo(({ context }) => context.lobby, ({ self, context }) => ({ type: 'JOIN', code: context.code, player: self, name: context.name })),
            ],
          },
        },
      },
      creating: {
        on: { JOINED: { target: 'waiting', actions: assign(({ event }) => ({ code: event.code, color: event.color, game: event.game })) } },
        after: { 15000: { target: 'lobby', actions: fail('the server did not answer') } },
      },
      joining: {
        on: {
          /* the game follows up with STATE (at once if it is running) */
          JOINED: { target: 'waiting', actions: assign(({ event }) => ({ code: event.code, color: event.color, game: event.game })) },
          SPECTATING: { target: 'watching', actions: assign(({ event }) => ({ code: event.code, color: null, game: event.game })) },
          NO_SUCH_GAME: { target: 'lobby', actions: fail('no game with that code') },
        },
        after: { 15000: { target: 'lobby', actions: fail('the server did not answer') } },
      },
      waiting: {
        on: { STATE: { target: 'playing', actions: fromState } },
      },
      playing: {
        on: {
          STATE: { actions: fromState },
          MOVE: {
            actions: sendTo(
              ({ context }) => context.game, /* a ref to the server's game actor, from JOINED */
              ({ self, event }) => ({ type: 'MOVE', from: event.from, to: event.to, promotion: event.promotion, player: self }),
            ),
          },
          REJECTED: { actions: assign(({ event }) => ({ error: `${event.move.from}→${event.move.to}: ${event.reason}` })) },
          RESIGN: { actions: sendTo(({ context }) => context.game, ({ self }) => ({ type: 'RESIGN', player: self })) },
        },
        always: { guard: ended, target: 'over' },
      },
      watching: {
        on: { STATE: { actions: fromState }, NEW_GAME: 'lobby' },
      },
      over: {
        on: { STATE: { actions: fromState }, NEW_GAME: { target: 'lobby', actions: assign({ code: null, game: null, color: null, fen: null, status: null, lastMove: null }) } },
      },
    },
  });
}
