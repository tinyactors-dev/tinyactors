# Multiplayer chess — server-hosted games, a lobby, join by code

A complete multiplayer game where every moving part is an actor, and the
browser and the server talk *actor to actor* through a link.

```sh
mise run example:chess                # builds the addon and the wasm, serves http://localhost:8080/
mise run example:chess --port 9000    # another port (--port 0: any free one)
```

Run it in `tinyactors-core`. The server stays in the foreground; Ctrl-C stops
it.

Open it in two windows: create a game in one, enter the five-letter code (or
open the share link) in the other. Open it a third time with the same code to
watch.

```text
 browser (web binding, wasm)                        server (Node.js, N-API binding)
┌────────────────────────────┐   link (WebSocket)  ┌───────────────────────────────────┐
│ player actor  ─────────────┼── CREATE/JOIN ─────▶│ lobby  (systemID "lobby")         │
│  (client/player.mjs)       │                     │   └ starts ─▶ game "game:K7QXN"   │
│        ▲                   │◀─ JOINED, STATE ────┤ game actor (actors.mjs)           │
│        └── MOVE/RESIGN ────┼────────────────────▶│   rules: chess.mjs (pure)         │
│ link status: connecting /  │                     │   idle games hibernate            │
│ open / reconnecting/closed │                     └───────────────────────────────────┘
└────────────────────────────┘
```

## How it works

- **The lobby** (`actors.mjs`, systemID `lobby`) answers `CREATE` by starting a
  game actor registered as `game:<CODE>`. It routes `JOIN {code}` to that
  game, or answers `NO_SUCH_GAME`. It keeps the list of games from what the
  games report (`GAME_OPENED`, `GAME_STARTED`, `GAME_OVER`, `GAME_GONE`).
- **A game** is one actor per board, moving through `waiting → playing → over
  → gone`. `MOVE {from, to, player}` passes only if the guard says it is that
  player's colour *and* a legal move (`chess.mjs`). Otherwise the player gets
  `REJECTED` with a reason. Every change is broadcast as `STATE` to both
  players and all spectators. A game left unplayed for 30 minutes ends as
  abandoned; a finished game stays visible for 10 minutes.
- **Players are addresses of actors in browsers.** A player is whoever sent
  the event (`_event.origin`, an address in the browser's system); the game
  answers with `<send targetexpr>` to that address, across the link. The
  lobby passes the player on when it forwards a `JOIN`. The client keeps
  the origin of `JOINED` as its game's address and then talks to its game
  directly. Neither side knows the other is remote.
- **The link status** is the router's view of the link (`link.status()`):
  the pill in the corner shows connecting, open with the round-trip time,
  reconnecting with the attempt and queue length, or closed. Moves made
  while the link is down are queued and delivered when it reopens.
- **Reloading** keeps you in the game. The client's system id is kept per tab
  (`sessionStorage`), so your actors keep their addresses. The client rejoins
  its game code, and the game recognizes the returning player.
- **Hibernation.** The server unloads idle actors (finished or quiet games)
  every minute. A move, a join or a timer brings a game back with its board,
  players and history.
- `/stats` shows the server system's numbers: actors, hibernated, wakes, links,
  games.

## Files

| File | |
|---|---|
| `chess.mjs` | rules: legal moves (castling, en passant, promotion, pins), check, mate, stalemate, 50-move and insufficient-material draws; FEN; perft-validated |
| `actors.mjs` | the lobby and game charts (pure; given a binding's API and a system) |
| `server.mjs` | the server: one tinyactors system, `serveLinks(system, { server, path: '/link', exports })` (browsers may send only the game's events), static files |
| `client/player.mjs` | the player chart (pure, also driven by the tests) |
| `client/app.mjs`, `client/board.mjs`, `client/index.html` | custom elements cloning `<template>`s: `<chess-app>`, `<chess-board>`, `<link-status>` |
| `test/chess.test.mjs` | perft: start position (depth 3), "Kiwipete" (depth 3: 97,862), en-passant endgame (depth 4), promotions; mate, stalemate |
| `test/game.test.mjs` | the whole flow over real WebSockets: the server on the N-API binding, three "browsers" on the wasm binding |

## Game records in PostgreSQL

With PostgreSQL running (`mise run pg:start`), the server keeps finished
games and every player's standing in an actor that lives **in the database**
([records.scxml](records.scxml), an SCXML document with the ECMAScript
datamodel), reached through a store link ([records.mjs](records.mjs),
`createStoreLink` with the dependency-free `tools/pgwire.mjs` client):

```
 browser ──(link)──▶ game (Node) ──(store link)──▶ records (PostgreSQL)
    ▲                                                     │
    └──────────── RECORD ◀── server relays ◀── outbox ◀───┘
```

When a game ends, the game actor sends `game.over` with both players'
addresses; the records actor updates its rows and sends each player
`RECORD {wins, losses, draws, games}` — addressed to the player actor in the
browser, relayed by the server, which announced only the database to the
browsers (`announce: [databaseID]`). The standings are plain rows:

```sql
SELECT datamodel->'standings' FROM tinyactors.actors WHERE name = 'records';
```

The server looks for PostgreSQL on 127.0.0.1, port 54320 (`TA_PG_PORT`),
database `tinyactors` (`CHESS_DB`). When nothing answers there, it says so
(`chess: no PostgreSQL on port 54320, no game records`) and runs without
records: the games work the same, and players get no `RECORD`.
`mise run example:chess --no-pg` leaves PostgreSQL out even when it is
running. `examples/chess/test/e2e.test.mjs` plays two games with records
(skipped when PostgreSQL is not running).
