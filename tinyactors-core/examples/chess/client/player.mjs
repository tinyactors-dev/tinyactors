/**
 * The browser side: one actor per player, a chart talking to the server's
 * lobby and game actors through a link (it does not know they are remote).
 *
 *   lobby → creating → waiting (show the code) → playing → over
 *         ↘ joining ↗ (JOINED)  ↘ watching            ↺ NEW_GAME
 *
 * The server knows the player by the address its events come from; the
 * player knows its game by the address JOINED comes from. Rejoining (after a
 * reload) is a JOIN from the same address: the game recognizes the player
 * and resends JOINED and STATE.
 *
 * UI events: CREATE {name}, JOIN {code, name}, MOVE {from, to, promotion},
 * RESIGN, NEW_GAME. Server events: JOINED, SPECTATING, NO_SUCH_GAME, STATE,
 * REJECTED; and RECORD {name, wins, losses, draws, games} — from the records
 * actor in PostgreSQL. Input: lobby (its address), name.
 */
export function playerChart({ scxml }) {
  const c = scxml.chart();
  const fromState = c.script(c.expr((s) => {
    const e = s._event.data;
    Object.assign(s, {
      fen: e.fen,
      status: e.status,
      names: e.names,
      lastMove: e.lastMove,
      moves: e.moves,
      spectators: e.spectators,
      color: e.you === 'spectator' ? null : e.you,
      error: null,
    });
  }));
  const fail = (message) => c.assign('error', JSON.stringify(message));
  const send = (type, data = () => ({})) => c.send(type, { targetexpr: 'String(game)' }, c.content(c.expr(data)));
  const toLobby = (type) => c.send(type, { targetexpr: 'String(lobby)' }, c.content(c.expr(({ code, name }) => ({ code, name }))));
  const timeout = [c.onentry(c.send('timeout', { delay: '15s', id: 'timeout' })), c.onexit(c.cancel('timeout'))];
  const joined = c.script(c.expr((s) => {
    Object.assign(s, { code: s._event.data.code, color: s._event.data.color || null, game: s._event.origin });
  }));

  return c.scxml('player', { initial: 'main', datamodel: 'ecmascript' },
    c.datamodel({
      lobby: null,
      name: 'Player',
      code: null,
      game: null, /* the address of the server's game actor */
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
    c.state('main', { initial: 'lobby' },
      c.transition('RECORD', undefined, c.assign('record', '_event.data')),
      c.state('lobby',
        c.transition('CREATE', 'creating',
          c.assign('name', '_event.data.name || name'),
          c.assign('error', 'null'),
          toLobby('CREATE')),
        c.transition('JOIN', 'joining',
          c.assign('name', '_event.data.name || name'),
          c.assign('code', 'String(_event.data.code).trim().toUpperCase()'),
          c.assign('error', 'null'),
          toLobby('JOIN'))),
      c.state('creating',
        ...timeout,
        c.transition('JOINED', 'waiting', joined),
        c.transition('timeout', 'lobby', fail('the server did not answer'))),
      c.state('joining',
        ...timeout,
        /* the game follows up with STATE (at once if it is running) */
        c.transition('JOINED', 'waiting', joined),
        c.transition('SPECTATING', 'watching', joined),
        c.transition('NO_SUCH_GAME', 'lobby', fail('no game with that code')),
        c.transition('timeout', 'lobby', fail('the server did not answer'))),
      c.state('waiting', c.transition('STATE', 'playing', fromState)),
      c.state('playing',
        c.transition('STATE', undefined, fromState),
        c.transition('MOVE', undefined, send('MOVE', ({ _event: { data } }) => ({ from: data.from, to: data.to, promotion: data.promotion }))),
        c.transition('REJECTED', undefined, c.assign('error', c.expr(({ _event: { data } }) => `${data.move.from}→${data.move.to}: ${data.reason}`))),
        c.transition('RESIGN', undefined, send('RESIGN')),
        c.transition(undefined, 'over', { cond: c.expr(({ status }) => !!status && !['waiting', 'playing', 'check'].includes(status.state)) })),
      c.state('watching', c.transition('STATE', undefined, fromState), c.transition('NEW_GAME', 'lobby')),
      c.state('over',
        c.transition('STATE', undefined, fromState),
        c.transition('NEW_GAME', 'lobby', c.script(c.expr((s) => {
          Object.assign(s, { code: null, game: null, color: null, fen: null, status: null, lastMove: null });
        }))))));
}

/* the player's screen state: the leaf of main */
export const screenOf = (value) => (typeof value === 'object' && value ? value.main : value);
