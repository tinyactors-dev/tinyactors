# Presence: an ant for every visitor

Every open tab of www.tinyactors.dev has an ant. The ants crawl in the
background of every page, all visitors on one shared field. They lay
pheromone trails, follow the trails of ants from their own country, and
find food. Tap another ant and yours goes to greet it. The visitor whose
ant is greeted reads "An ant from Estonia greets you." A floating ant
button holds the site's menu and a switch to leave.

The feature is a demo of tinyactors itself, on both sides of the
connection:

- **On the server**, each tab's ant is an actor on one tinyactors host. The
  field the ants walk is that host's I/O processor.
- **In the browser**, the connection, the participation switch and the
  greeting line are statecharts on `@tinyactors/wasm`. Disconnects,
  retries and a hidden tab are transitions, not flags.

No Durable Object or other hosted actor service is involved: the point is
to show ours.

## Decisions

Decided by the maintainer, 2026-11-21:

- **One world** for the whole site. Every page shows the same field.
- **On by default.** A visitor's ant joins when the page loads. The
  switch in the ant menu takes it out, and that choice is remembered in
  the browser.
- **tinyactors in the browser too.** The page's control flow is
  statecharts, so disconnects and reconnects are handled in one place.
- **Server on exe.dev**: a VM named `ta-presence`, tagged `tinyactors`.
  The page connects to `wss://ants.www.tinyactors.dev`. The name is a
  subdomain of www because the feature is part of the website.

## How it works

```
browser tab                          Cloudflare Worker            exe VM ta-presence
──────────────────────────────       ─────────────────            ───────────────────────────────
link.scxml (@tinyactors/wasm)  ─GET─▶ /presence/ticket
  ticketing                    ◀───── {country, exp, nonce}+HMAC
  connecting ──────────────── WebSocket wss://ants.www.tinyactors.dev ─────▶ server.mjs
  joined   ◀──── trips, field, greetings (JSON) ──────────────────────────── host: visitor.scxml × tabs
  hidden / reconnecting / off                                                   world.mjs (urn:world)
canvas layer: ant.mjs (planted feet), Path2D of the rig
```

### The browser

- **`presence/link.scxml`** runs in the page on `@tinyactors/wasm`. It is
  the vendored copy the anthill already loads, imported only after the
  page has loaded. It holds the connection's life:
  - states `off`, `ticketing`, `connecting`, `joined` (with `visible` and
    `hidden` inside), `backoff` and `gone`;
  - its I/O processors are the WebSocket, the ticket fetch, the canvas
    layer and the status line;
  - a lost socket goes to `backoff`, with delays from 1 s doubling to
    60 s, then back to `ticketing`;
  - `visibilitychange` sends `hide` and `show`;
  - the switch sends `enable` and `disable`.
- **`presence/layer.mjs`** draws every ant on one fixed full-viewport
  canvas behind the content.
  - It uses Canvas 2D, with `Path2D` objects built once from the rig's
    paths, posed by the ant-animation skill's gait
    (`.agents/skills/ant-animation/scripts/ant.mjs`, copied into
    `presence/`).
  - Positions on the wire are fractions of the field (0 to 1 on each
    axis). Each browser maps them to its own viewport and computes the
    gait in pixels, so ants are never stretched. The layer does not scroll
    with the page.
  - It draws at most 60 ants, its own always among them.
  - It stops drawing while the tab is hidden. With
    `prefers-reduced-motion`, the ants stand still.
  - The canvas is `aria-hidden`.
- **Flags** are drawn clipped to each ant's gaster and half transparent.
  Unknown (`XX`) and Tor (`T1`) visitors get none.
- **Tapping:** a document-level `pointerup` counts only when it lands
  outside any link, button, form field or selected text, and within
  22 px of an ant. The menu has "Greet an ant" for keyboards and screen
  readers.
- **The ant button** floats at the bottom right. It opens a popover with
  the site's navigation (the same `nav-links` template as the header) and
  the ants switch (`role="switch"`). The greeting line sits in it as a
  polite live region and also shows briefly beside the button. Its words
  live in `content/site.xml`. It fades out while it would cover an
  element marked `data-presence-aside` (the live chat's window), unless
  it has focus or its menu is open.

### The server

- **`presence/server.mjs`**: Node.js 22 with `@tinyactors/node` 0.4.0 (the
  version the examples pin) and `ws`. It listens on port 8000, behind
  exe's HTTPS proxy.
- **One actor per tab**, `presence/visitor.scxml`, spawned under the tab's
  name:
  - `wandering`, `following` (its country's trail) and `foraging`, chosen
    by what the world says it smells;
  - `eating`: a trip that ends at a pile of food arrives as `found`; the
    ant takes a bite (the world shrinks the pile and removes it after the
    last), chews 2.5 s, and is full for 30 s;
  - `approaching` (a greeting asked for) and `greeting`, which a tap
    starts, or a `bump`: two roaming ants that meet greet without a tap,
    one picked by the world as the greeter, then neither for 30 s;
  - `greeted`, which tells the tab who greeted it;
  - `asleep`, while the tab is hidden: the server hibernates the actor
    there, and the world keeps its position and draws it dimmed;
  - `leaving` (final), when the socket has been gone 20 s or the switch
    is turned off.
- **The world, `presence/world.mjs`**, is the actors' I/O processor
  `urn:world`, as in the anthill. Ants decide only when something happens
  to them; the world plans trips (two points, two times) and says when an
  ant arrives. It keeps:
  - one pheromone grid per country, deposited along trips, evaporating;
  - five piles of food (24 units, 8 a bite) whose scent spreads; a hungry
    ant within smelling distance walks to the nearest; a finished pile
    grows again elsewhere 20 s later, and every change goes to the tabs at
    once (`food`), so a pile vanishes when it is eaten;
  - the positions of every ant, asleep ones included.
- **A greeting** is a message between actors. A tap asks the visitor's
  actor to greet a handle, and the world plans trips toward the target,
  re-planned on each arrival. In reach, the actor `<send>`s `greeting` to
  the other actor's address. The other actor enters `greeted` and tells its
  tab the greeter's country. The greeting line names the country with
  `Intl.DisplayNames`. One greeting per tab may start every 10 s.
- **The wire** is JSON over one WebSocket:
  - a tick 10 times a second sends each client the trips started since
    the last tick, for the ants it draws;
  - the pheromone and food fields go out as a coarse grid every 2 s;
  - ants are short per-connection handles; tab ids never leave the server.

### Identity, country and abuse

- **Tab id:** random, made per page load. It is kept in `sessionStorage`
  only so that a reload within the 20 s grace finds the same ant. A second
  live connection with an id already connected is refused, because
  duplicating a tab copies `sessionStorage`; the page then makes a new id.
- **Country:** the site's Worker answers `GET /presence/ticket` with
  `{country, exp, nonce}` signed with HMAC-SHA-256, taken from
  `request.cf.country`. The server accepts each nonce once and before
  `exp` (60 s). The server keeps no GeoIP database.
- **The server checks** `Origin` (www.tinyactors.dev only). It caps
  connections per IP, the field's population and greetings per IP, with
  addresses read from `X-Forwarded-For`, held in memory and never logged.
- **Failure:** the page works the same without the server. The layer
  fades in once joined.

## Where things are

| Path | What |
|---|---|
| `presence/visitor.scxml` | the server's actor per tab |
| `presence/link.scxml` | the browser's connection chart |
| `presence/world.mjs` | the field: trips, pheromones, food, greetings (takes the tinyactors package as an argument, so it runs under Node and in tests) |
| `presence/protocol.mjs` | the messages both sides send |
| `presence/ticket.mjs` | signing and checking tickets (WebCrypto: Worker, Node.js, browser) |
| `presence/server.mjs`, `package.json` | the server the VM runs |
| `presence/client.mjs`, `layer.mjs`, `ant.mjs` | the browser side |
| `worker.mjs` | the site's Worker: `/presence/ticket`, everything else static assets |
| `examples/presence/` | the world and the visitor chart on a simulated clock, checked against `output.txt` by `mise run test` |
| `mise/tasks/presence/*` | deploy to the VM, set the ticket key |

## Infrastructure

- **VM:** `ta-presence` on exe.dev, Ubuntu 24.04, tag `tinyactors`, public
  HTTPS proxy on port 8000. Its API is `POST https://exe.dev/exec`, with
  `EXE_API_KEY` read through fnox from 1Password (`tinyactors-exe-api-key`).
- **DNS:** `ants.www.tinyactors.dev`, a CNAME to `ta-presence.exe.xyz`,
  **DNS only** (exe issues the certificate; a proxied record breaks it).
  Then `domain add ta-presence ants.www.tinyactors.dev` on exe.
- **Ticket key:** one random secret, a Worker secret (`TICKET_KEY`) and an
  environment variable on the VM, set together by `mise run
  presence:key`. To rotate it, run that again.

## Status

As of 2026-11-21:

- **Verified:**
  - The server runs on the VM, passes `presence/smoke.mjs` through exe's
    public proxy (`wss://ta-presence.exe.xyz`), and keeps an idle
    WebSocket open (6 minutes held, 15 pings answered).
  - Three Chromium tabs against a local server join, greet (the greeted
    tab reads "An ant from Estonia greets you."), and switch off, with no
    console errors.
- **Live** on www.tinyactors.dev since 2026-11-21: the Worker issues
  tickets, and two Chromium tabs on the live site joined and greeted
  ("An ant from United States greets you.") with no console errors.
- **The domain** `ants.www.tinyactors.dev` is registered on exe (DNS
  only, exe's certificate) and the page connects to it.
- **Order of a first deploy:** `mise run deploy` (the Worker answers 503
  for tickets until it has a key, and the page backs off), then `mise run
  presence:key`.
- **Not measured:** the cost of a busy field. At the page's ant size, many
  flags look alike (tricolours).
