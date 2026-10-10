# Torture chamber

`torture-bench` pushes tinyactors to its limits and shows what happens, live.

```sh
mise run torture              # the scenario home page (release build) on :8090 ($PORT)
mise run torture chaos        # start a scenario right away
mise run torture:test         # every scenario headless + the dashboard/WebSocket tests
```

```
torture-bench [SCENARIO_DIR] [--scenarios DIR] [--duration S] [--interval MS] [--scale F]
              [--out FILE] [--port N] [--host ADDR] [--web DIR] [--headless] [--loop]
              [--seed N] [--quiet] [--param KEY=VALUE]...
```

- The home page (`/`) shows a card per scenario in `--scenarios` (default: the
  scenario's parent directory, else `tools/torture/scenarios`); a card opens
  `chamber.html?scenario=<id>`, which switches the bench to that scenario. One
  bench runs one scenario at a time — every open dashboard follows a switch.
  The server keeps running after a scenario finishes ("Run again").

- `--scale` multiplies actor counts and event rates (scenarios use `bench.scaled(n)`).
- `--out FILE` writes the time series as JSON lines; the same lines stream over
  the WebSocket at `/ws`. `--headless` skips the server.
- `--loop` keeps running past the scenario duration (dashboard demos).
- `--param KEY=VALUE` (repeatable) reaches scripts as `bench.params` (numbers parsed).
- `--param tracing=disabled|records|otlp` (and `traceDetail=summary|decisions|full`,
  `traceSample=0..1`: the share of the traces each system starts that is sampled)
  sets every system's tracing: `records` (the default) reads and counts each
  record after every slice, `otlp` also encodes them, `disabled` turns tracing
  off in the library (`TA_TRACING_DISABLED`). Samples report `traceRecords`,
  `traceBytes` and `otlpBytes` per second, `traceBusy` (the share of time
  spent draining), and `traceDropped`; a scenario's `config` takes the same
  `tracing`, `traceDetail` and `traceBytes`.
- Exit status 1 when an expectation fails or a script throws.

## Architecture

```diagram
┌──────────────────────────── torture-bench (one thread) ─────────────────────────────┐
│                                                                                     │
│  scenario scripts (quickjs)          systems (ta_system + own ECMAScript runtime)   │
│  ┌──────────────────────┐  spawn/   ┌──────────┐  tinyactors://b/#_scxml_7  ┌──────┐ │
│  │ prelude.js  bench.*  │──send───▶ │ system a │ ─── TA_EFFECT_SEND ──router──▶ │  b   │ │
│  │ setup / generators / │           └──────────┘ ◀── _event.origin ─────── └──────┘ │
│  │ mutations            │◀── <send type="bench"> (results, latencies) ──────────────│
│  └──────────────────────┘                                                           │
│          │ metrics                 ta_system_stats_get() every --interval            │
│          ▼                                                                          │
│   JSON lines ──▶ --out file                                                         │
│              └─▶ HTTP + WebSocket (/ws) ──▶ web/torture dashboard                    │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

- **Systems** are independent `ta_system`s, each with its own quickjs-ng
  datamodel runtime (like separate processes). Charts may also use the null
  datamodel.
- **Cross-system addressing** uses the runtime's `TA_IO_PROCESSOR_SCXML_REMOTE`
  processor: a chart sends to `tinyactors://<system>/#_scxml_<session>`; the
  bench routes the `TA_EFFECT_SEND` to the other system (payload re-encoded as
  JSON) with `origin` set, so replies work via `_event.origin`. Unknown systems
  or sessions answer the sender with `error.communication`.
- **Charts talk to the bench** with `<send type="bench" event="...">`; scripts
  receive them with `bench.on(...)`.
- **The `system` I/O processor** (`type="system"` or
  `http://tinyactors.dev/torture/#SystemEventProcessor`) lets charts create
  actors. SCXML's own processor only reaches existing sessions (§6.2.4,
  Appendix C.1: a nonexistent session is `error.communication`; sessions come
  from `<invoke>`), but platforms may add processors (§6.2.5), so this lives in
  the test runtime, not the library:
  `<send type="system" event="spawn" target="chart.scxml" id="…">` + `<param>`s
  starts a top-level session of that chart with the params as its initial data;
  when it finishes the sender receives `done.spawn.<sendid>` (`done.spawn`
  without an id) with the donedata and `origin = #_scxml_<id>`.
  `event="terminate"` with `target="#_scxml_<id>"` stops a session. Unknown
  charts/sessions raise `error.communication`.
- **Scenario I/O processors**: `"ioprocessors": ["mail", "store"]` in
  `scenario.json` registers host processors of those types in every system;
  scripts implement them with `bench.ioprocessor(type, msg => …)` and answer with
  `bench.send(msg.from, …)` or `bench.failSend(msg)` (error.communication).
- Effects that act on the same system (spawns, replies, script callbacks) run
  after the effect batch is acknowledged, as `tinyactors/system.h` requires.
- **One event loop** (`loop.c`) owns `poll()`, registered HTTP/WebSocket and
  PostgreSQL descriptors, monotonic deadlines, and a SIGCHLD self-pipe. There
  are no blocking PostgreSQL calls or child waits on the server thread.
  Script timers and QuickJS Promise jobs run in slices of at most 32 callbacks
  or 2 ms; actor systems run round-robin, one 4,000-fuel slice at a time. I/O
  is polled between slices. Callbacks themselves are cooperative: a long
  synchronous JavaScript function must split its work across timers.
- **Clocks**: loop deadlines never reset. `bench.now()` and JSONL `t` remain
  scenario-relative milliseconds and seconds, respectively; actor-system
  deadlines are converted to the loop clock only at the scheduling boundary.
- **Scripts** run in a separate QuickJS context, with scenario file reads and
  the asynchronous PostgreSQL API below. Switching scenarios closes pending
  queries and discards the old context and its jobs. Normal completion awaits
  in-flight async timer callbacks, finish hooks and checks while still serving
  I/O and sampling. JSONL output and pass/fail checks keep the same meaning.
- **Children**: pgbench runs through `mise exec` in a child (toolchain discovery
  never blocks the loop). Exits are reaped on SIGCHLD, even without a dashboard.
  Stopping loads signals their process groups with SIGTERM and schedules
  SIGKILL after one second; reaping and temporary-script removal are asynchronous.

## Scenarios

| Scenario | Pushes on |
|---|---|
| `throughput` | Raw delivery: null-datamodel ping-pong storms (static invoke routing) + ECMAScript pingers answered via `_event.origin` |
| `tracing` | The `throughput` storms with tracing at full detail: after every slice the bench encodes every system's trace records as OTLP protobuf (`ta_otlp`) and counts the bytes, as a host posting them would (ADR 0006) |
| `crowd` | 100,000 mostly idle actors, growth by relocation, timer heap, history-based (Markov) events |
| `heavy-data` | Large ECMAScript datamodels (20k-entry catalogs, journals of schema-generated nested orders), `<foreach>`, bulk imports |
| `federation` | Three systems addressing each other's actors; kill/restore a system; add a system at run time |
| `capacity` | Where the limit is: per messaging pattern, grows the actor count, then bisects to the largest healthy count (latency, throughput, the memory budget or the 8.4 M-per-system runtime limit — see below) |
| `campaign` | 50,000 emails: one actor per delivery attempt via the `system` processor, an idempotent mail processor with key retention, checkpointed progress, crash/restart, outages, a stale restart that waits for an operator |
| `fit` | How many small ECMAScript session actors (a set of ids + a resettable timeout) fit on this machine: grows until 80% of the available memory (or the cgroup limit) or the latency SLO |
| `hibernate` | 2 million ECMAScript sessions with a few thousand resident: the host sweeps for cold actors, unloads them to a blob file and wakes them on demand (skewed hot/random touches, flash crowds) |
| `reload` | CGI-style hot reloading under traffic: new chart versions every few seconds, new workers on the newest one, old versions drain and are freed, residents migrate live (`error.platform.upgrade` with their previous bindings) — no job lost or duplicated, counts continuous, timers kept |
| `postgres` | Durable actors in PostgreSQL ([bindings/postgres](../../bindings/postgres)): 2,000 bank accounts, concurrent pgbench transfers with actor-to-actor deposits, a scheduler and an outbox worker as the application would run them, money conserved at every sample, a hot spot, a database restart in the middle (needs the PostgreSQL service — see below) |
| `postgres-millions` | Millions of durable session actors in PostgreSQL, almost all idle with a pending timeout, a few thousand hot: the population grows to 2,000,000 under traffic (in its own database, kept between runs); hot events, cold events and `run()` stay as cheap as with a thousand; flash crowd, cold sweep, one very hot actor, restart |
| `chaos` | Eventless infinite loops, invoke churn, timer storms with cancel, a hot mailbox, 48-level hierarchy, 64 parallel regions, an error storm — with a latency probe for fairness |

### Capacity search

`mise run torture capacity` (or `torture-bench tools/torture/scenarios/capacity`)
determines, per messaging pattern, the largest actor count this machine
sustains — and what stops it:

| Pattern | Traffic per actor | |
|---|---|---|
| `idle` | none | null datamodel |
| `heartbeat` | 1 event/s | a 1 s delayed send to itself (one timer per actor) |
| `cells` | 17/9 events/s | a parent invokes 8 children, ticks them every second, each replies |
| `gossip` | 2 events/s | ECMAScript; a random peer by session address about once a second |
| `ingress` | 1 event/s | the host sends to random actors |

Each step spawns toward the target in adaptive chunks, settles (1 s), then
measures (2 s): probe latency (a probe actor answers every 100 ms), delivered
events versus the pattern's expected rate, faults. A step is healthy when the
probe p99 ≤ `sloMs` (100), ≥ `minRate` (85%) of the expected events arrive and
nothing faulted. While healthy, the target grows by `fastGrowth` (×2) far from
the limit and by `growth` (×1.25) close to it — but never past what fits the
**memory budget**: 80% of the memory available at start, or of the cgroup limit
when tighter (`memoryFraction`, `memoryMB`), measured as resident memory with
room for the region copy while the system grows. When a size fails twice, the
limit lies between the last healthy and the failing count, and the search
**bisects** that bracket until it is within `tolerance` (5%). A smaller midpoint
is measured on a fresh system: an overloaded one does not come back on its
own (see below). A pattern also ends at the runtime's hard limit of
**8,388,606 actors per system** (`TA_SYSTEM_MAX_ACTORS`: 24-bit handles, one
bit lent to external invoke tokens — more actors means more systems), at
`maxActors` if set, or after `phaseSeconds` (900). Results — capacity, the unhealthy
count above it, events/s, probe p99, resident bytes per actor, how many would
fit the memory budget, the reason — appear as `capacity.*`, `probeP99Ms.*`,
`servedPct.*` and `rssMB.*` series and as a table at the end.

```sh
torture-bench tools/torture/scenarios/capacity --param patterns=heartbeat,gossip \
  --param sloMs=20 --param tolerance=0.02
```

Reference run (release build, 4-core orb, one thread, 6 GB cgroup → 4.9 GB
budget, defaults; ~5 minutes):

| Pattern | Limit | Unhealthy at | Events/s | Probe p99 | Resident B/actor | Fits in memory | Stopped by |
|---|---|---|---|---|---|---|---|
| idle | 8,192,000 | — | 0 | 0.3 ms | 141 | 36.6 M | runtime limit (8,388,606 per system) |
| heartbeat | 1,520,000 | 1,560,000 | 1,496k | 70 ms | 235 | 21.8 M | latency |
| cells | 836,721 | 861,336 | 1,528k | 40 ms | 197 | 26.0 M | latency |
| gossip | 100,000 | 104,000 | 197k | 31 ms | 3,066 | 1.66 M | latency |
| ingress | 496,000 | 512,000 | 490k | 17 ms | 144 | 33.4 M | latency (host script loop) |

Everything with traffic is CPU-bound on the one thread long before memory
(1.5–1.7 M null-datamodel events/s; ~200k ECMAScript events/s for `gossip`,
which needed 60 KB per session and stopped at 12,208 before the shared realm).
Repeated runs vary by up to ~10% (heartbeat: 1.52–1.73 M). Idle actors are
bound by the handle width, not memory. `ingress` is bound by the bench's own
script sending every event.

**Overload is sticky (metastable).** A heartbeat re-arms 1 s after it was
processed. During an overload the backlog is processed in one burst, so the
timers re-armed in it stay clustered: each period becomes 1 s + the time to
drain the cluster, and the system keeps missing its rate at loads it sustains
when the timers are spread (measured: 1.54 M heartbeat actors at 56% served
after an overload at 2 M, 101% on a fresh system). Hosts that need to recover
from overload should shed load or add jitter to periodic delays.

### Fit

`mise run torture fit` answers "how many actors fit on this machine?" for the
common session shape: an ECMAScript datamodel holding a small set of ids, and a
timeout as a delayed send to itself that every touch cancels and re-arms. The
budget is 80% of the memory available at start, or of the cgroup limit when that
is tighter (`--param memoryFraction`, `memoryMB`); the search grows until the
next step would exceed it, the probe p99 passes `sloMs`, or touches stop being
processed. Reference run (release build, 6 GB cgroup limit → 4.9 GB budget):
**500,000 sessions healthy** (probe p99 40–66 ms), 2.9 KB each — 2.4 KB of
ECMAScript heap (a scope object in the shared realm) and 0.44 KB in the actor
system; ≈ 1.8 million would fit in the budget, so the search stops at the
latency SLO: at 625,000 sessions the touches alone (10 %/s ≈ 62,500 events/s)
saturate the single thread. With one JS context per session
(`TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED`) the same run stopped at 80,000 sessions,
60.7 KB each, by memory. With the null datamodel an actor costs ~0.4 KB (see
the capacity table).

### Hibernate

`mise run torture hibernate` runs more sessions than fit in RAM. The library
never does I/O; the bench is the host: every 500 ms it asks for cold actors
(`ta_system_cold_actors`, a CLOCK sweep over one revolution) and hibernates
them (`ta_actor_hibernate`) into an append-only file (unlinked at creation).
An event for a hibernated session — a touch, its timeout — waits in its
mailbox; the system emits one `TA_EFFECT_WAKE`, the bench reads the blob and calls
`ta_actor_wake`. Touches are skewed: 90% to a hot set of 1% of the sessions, the
rest to random (mostly hibernated) sessions; one in 50 replies, measuring hot
vs random latency. Reference run (release build, defaults, 120 s):

| Sessions | Hibernated | Peak resident | RSS | Per session | Blob | Wakes | Touch p99 hot / random |
|---|---|---|---|---|---|---|---|
| 2,000,000 | 1,980,699 | 43,917 | 524 MB | 273 B | 343 B | 365k (~6.8k/s) | 135 / 176 ms |

A resident ECMAScript session costs ~2.9 KB (see Fit); hibernated, what stays
is the 48 B actor record, its session-table entry and its pending timeout (a
timer and its event) — ~250 B, most of it the timer. The ramp spawns ~30k
sessions/s while sweeping; latency includes the bench's own 50 ms batching and
the sweeps over 2 M slots.

### Reload

`mise run torture reload` changes the worker chart while ~4,000 jobs/s flow
through a router to workers by SCXML address. Each `bench.reload` compiles a
new version: the bench is a plain `ta_host` client (include/tinyactors/host.h) —
each version is a segment of the system's host, sharing its base URI and
ECMAScript runtime with a session range of its own; new workers start in the
newest one, sends between versions are routed inside the library, and a
version is freed once its last worker finishes. `bench.migrate` moves the
residents (export → translate states by id → adopt with the same session id)
as a job of ~2 ms steps between the systems' runs, never in one blocking pass;
before the first migration some residents are hibernated and the job wakes
them (`wake: true`);
each receives `error.platform.upgrade` with its previous bindings, from which
version 2 takes over its renamed counter. With `--param watch=1`, saving
`charts/worker-next.scxml` loads it as the next version (CGI style) and
migrates the residents. Reference run (release build, defaults, 30 s):

| Jobs | Lost | Duplicated | Versions | Migrated | Upgrades with bindings | Versions freed |
|---|---|---|---|---|---|---|
| 102,207 | 0 | 0 | 4 | 1,200 | 1,200 | 3 |

### PostgreSQL

`mise run torture postgres` puts the PostgreSQL binding under load on the
instance `mise run pg:start` runs in this machine (127.0.0.1:54320, database
`tinyactors`; in an orb the supervised service `tinyactors-pg`). The scenario
declares `"services": ["postgres"]`, so the bench probes the instance every
second while a dashboard is open (or a start is in progress) and pushes its
state; the home page and the chamber show it live — not running, not set up,
starting, available — with a **Start PostgreSQL** button that runs
`mise run pg:start` (idempotent: initdb, start, `CREATE DATABASE`/`EXTENSION`).
A run that starts while PostgreSQL is down waits for it.

What runs where, as in a real deployment:

| Process | Role | SQL |
|---|---|---|
| 8 pgbench clients | the application's request path | `tinyactors.send(account, 'transfer', {to, amount})` |
| 1 pgbench client | the scheduler | `tinyactors.run(1000)` every 200 ms: due timers, deferred deposits |
| 1 pgbench client | the audit worker | `tinyactors.ack(id) FROM tinyactors.claim(500, …)` every 200 ms |
| torture-bench | measurement only | one snapshot per second (money, queues, events, late timers) + a one-event latency probe |

A transfer debits the sender and sends `deposit` to the receiver — processed
in the same transaction when the receiver's row is free, otherwise left in its
inbox (`SKIP LOCKED`) for `run()`. Money is conserved when the balances plus
the deposits still in inboxes, read in one snapshot, equal the opening total —
checked every second, including across the restart. Reference run (4 CPUs,
PostgreSQL defaults, 70 s, hot spot at 25 s, restart at 55 s; the work is in
PostgreSQL, so the bench's build type does not matter):

| Events | Per second (avg / peak) | Max inbox | Money checks | Drift | Restarts | Audit effects |
|---|---|---|---|---|---|---|
| 515,170 | 7,359 / 12,102 | 23 | 67 | 0 | 1 | 2,035 |

`torture:test` runs it when PostgreSQL answers and skips it otherwise (CI
covers the binding with `mise run pg:test`).

`mise run torture postgres-millions` is the scale counterpart: a durable
population of session actors (a set of ids, a timeout a day after the last
touch — every idle actor has a pending timer) in a database of its own,
`tinyactors_millions`, kept between runs (~600 bytes per actor;
`--param reset=1` starts over; `--param database=NAME` isolates another run).
The responsiveness test uses the disposable `tinyactors_torture_test` database,
not the retained `tinyactors_millions` population. Four pgbench spawners grow it to 2,000,000
(`--param population`) while 6 clients touch a hot set of 10,000
(`--param hot`), 2 clients touch random actors anywhere, and a scheduler calls
`run()`. The bench measures with cheap queries only and probes one hot event,
one cold event and one `run()` per second. Reference (4 CPUs, PostgreSQL defaults):

| Phase | Actors stored | Spawns/s | Events/s | Hot event | Cold event | `run()` |
|---|---|---|---|---|---|---|
| ramp (0 → 2 M, all clients on 4 CPUs) | 2,002,000 after 86 s | 23,700 | 4,100 | | | |
| steady, 2 M stored | 2,002,000 | — | 7,400 | 0.86 ms | 0.71 ms | 0.21 ms |

Latencies include the round trip; `mise run pg:scale` measures the same
without the dashboard, up to 5 million (bindings/postgres/README.md).

A scenario directory:

```
scenario.json   systems, initial actors, scripts, generators, mutations, schedule, expect
charts/         SCXML files, or scripts returning SCXML text ({ "name", "script", "params" })
scripts/        run once at start
generators/     schema- or history-based event sources
mutations/      change the running system (schedule or dashboard button)
data/           JSON schemas, JSONL histories
```

`scenario.json`:

```json
{
  "name": "federation",
  "title": "Federation — actors addressing actors in other systems",
  "description": "shown on the dashboard",
  "duration": 40,
  "systems": { "billing": { "charts": ["charts/account.scxml"], "config": { "maxActors": 256, "maxEvents": 8192, "quantum": 300, "jsMemoryMB": 512 } } },
  "spawn": [{ "system": "billing", "chart": "account", "count": 16, "group": "accounts", "data": { "balance": 100 } }],
  "scripts": ["scripts/topology.js"],
  "generators": ["generators/orders.js"],
  "mutations": { "kill-billing": { "title": "Kill billing", "description": "…", "script": "mutations/kill-billing.js" } },
  "schedule": [{ "at": 12, "mutation": "kill-billing" }],
  "expect": ["totals.remoteOut > 100", "counters.confirmed > 0", "totals.faults == 0"],
  "services": []
}
```

`"services": ["postgres"]` marks a scenario that needs an external service:
the dashboard shows the service's live state and a start button, and hides the
in-process system charts when the scenario has no systems.

Spawn counts are multiplied by `--scale` unless `"fixed": true`. `spawn.data`
initializes the chart's `<data>` (ECMAScript datamodel). Expectations compare a
path in the final summary (`systems.<name>.<stat>`, `totals.*`, `counters.*`,
`metrics.<histogram>.p99`, …) with a number; scripts can add
`bench.check(description, summary => boolean)`.

## The `bench` API (scripts)

| Call | |
|---|---|
| `bench.addSystem(name, { charts, config })`, `removeSystem(name)`, `systems()`, `stats(name)` | systems at run time |
| `bench.spawn(system, chart, { count, group, data, name })` → ids | `data` may be `(i) => data` |
| `bench.send(id, event, data)`, `broadcast(ids, event, data)`, `terminate(id)`, `alive(id)` | |
| `bench.address(id)` | SCXML target URI of an actor (usable from any system) |
| `bench.group(name)`, `prune(name)` | actor groups |
| `bench.every(ms, fn)`, `after(ms, fn)`, `cancel(t)`, `rate(perSecond, fn(n))` | timers, rate-driven generators |
| `bench.on(event, (data, fromID) => …)` | events from charts (`<send type="bench">`) |
| `bench.schema.load(path)`, `schema.generate(schema, rng)` | JSON-Schema-subset generator: types, `properties`, `required`, `enum` (+ `x-weights`), `const`, `oneOf`/`anyOf`, `$ref`/`$defs`, `minimum`/`maximum`, `minLength`/`maxLength`, `minItems`/`maxItems`, formats `uuid`, `date-time`, `email` |
| `bench.history.load(path)` | JSONL history `{ "t": seconds, "event": "...", "data": {…}, "<key>": … }` |
| `bench.history.markov(history, { order, key, jitter })` → `{ next(streamID) }` | learns event sequences per key, inter-arrival gaps and payload samples (numbers perturbed by `jitter`) |
| `bench.history.replay(history, fn, { speed, loop })` | replays recorded timing |
| `bench.histogram(name).record(v)`, `metric(name, v)`, `count(name, n)` | custom series (p50/p95/p99/max per sample) |
| `bench.rng(seed)`, `bench.random` | deterministic PRNG (`int`, `pick(arr, weights)`, `normal`, `exp`) |
| `bench.mutate(id)`, `mutation(id, fn)`, `check(desc, fn)`, `onFinish(fn)`, `log(...)`, `stop(msg)` | |
| `bench.params`, `bench.result(row)`, `bench.describeMetric(prefix, { title, unit, format })` | `--param` values, rows of the result table, chart labels |
| `bench.ioprocessor(type, fn)`, `bench.failSend(msg)` | scenario I/O processors (`"ioprocessors"` in scenario.json) |
| `bench.reload(system, charts)` → version, `migrate(system, { wake, timeoutMs, onDone })`, `migration(system)`, `versions(system)`, `watch(system, paths, { ms, load, migrate, then })`, `mtime(path)` | hot reload: a new version (charts as paths or `{ name, text }`); live migration as a job the bench loop advances in ~2 ms steps (actors walked with `ta_system_actors` in chunks; busy ones retried, hibernated ones woken with `wake`, stragglers left behind after `timeoutMs`; `onDone({ migrated, failed, pending })`); CGI-style file watching |
| `bench.duration()` | the run's planned duration (s) |
| `bench.pg.status()`, `available()`, `query(sql)` → Promise of rows, `load({ clients, seconds, script })` → pid, `stopLoad()`, `use(db)`, `start()`, `restart()` | the PostgreSQL service: cached asynchronous probe state, FIFO queries on the scenario's connection (columns as text or null), pgbench scripts in the background (at most 16 active; stopped with the scenario), the database later queries and loads use (await outstanding queries before `use`; back to `tinyactors` when it stops), `mise run pg:start` / `pg:restart` |
| `bench.machine`, `bench.rss()` | this machine's memory (total, available, cgroup limit), CPUs; the bench's resident memory |

Each script file runs in its own block scope; share state through `globalThis`.

Use `await bench.pg.query(sql)` inside an async timer or finish/check callback.
It resolves after PostgreSQL's ReadyForQuery, or rejects with an Error on SQL
errors, connection failure or timeout (1 s to connect, 30 s per query). The
queue is capped at 128 requests. The probe uses its own connection and a
300 ms connect timeout. Both use `127.0.0.1:${BENCH_POSTGRES_PORT || 54320}` and trust
authentication. `status()` never waits for a probe.

```js
bench.every(1000, async () => {
  const [row] = await bench.pg.query('SELECT count(*) AS n FROM tinyactors.inbox');
  bench.metric('queues.inbox', Number(row.n));
});
bench.check('outbox drained', async () => {
  const [row] = await bench.pg.query('SELECT count(*) AS n FROM tinyactors.outbox WHERE delivered_at IS NULL');
  return Number(row.n) === 0;
});
```

`bench.every` skips ticks while its previous returned Promise is pending;
`bench.after`, `onFinish` and `check` also accept async callbacks. Rejected timer
callbacks count as script errors; rejected or false checks fail validation.
Do not detach asynchronous work that the final summary must wait for: return
its Promise from the callback.

The server regression tests issue HTTP and WebSocket round trips every 20 ms
(including while earlier requests are outstanding) and require p99 below
50 ms during chaos, a PostgreSQL black hole, and a fresh 100,000-actor SQL
prepopulation. `TORTURE_BENCH=/path/to/binary` selects a binary for comparison.

## Time series and protocol

One JSON object per line / WebSocket text frame:

- `{"type":"sample","t":12.25,"systems":[…],"totals":{…},"metrics":{…},"script":{…},"paused":false}`
  — per system: `actors`/`actorsCap`, `runnable`, `events`/`eventsCap`,
  `timers`/`timersCap`, `workspaces`, `slabBytes`, `regionBytes`, rates per
  second (`delivered`, `fuel`, `turns`, `timersFired`, `remoteOut`,
  `remoteIn`, `logs`, `toBench`, `hostSent`), `busy` (fraction of wall clock
  spent in `ta_system_run`), `jsHeap`, totals (`deliveredTotal`, `done`,
  `faults`, `remoteFailed`).
- `{"type":"log","t":…,"level":"info|warn|error|mutation|chart","source":…,"message":…}`
- `{"type":"summary",…,"ok":true,"failures":[]}` at the end.
- On connect: `{"type":"hello","scenario":{…mutations…}}` and
  `{"type":"history","samples":[…],"logs":[…]}` (last 10 minutes).
- `{"type":"service","name":"postgres","state":"unknown|unavailable|starting|missing|available|failed","detail":…,"version":…,"extension":…,"address":…,"start":"mise run pg:start"}`
  — on connect and on every change (probed once a second while a dashboard is connected).
- Client → bench: `{"cmd":"open","scenario":"<id>"}` (switch), `{"cmd":"restart"}`,
  `{"cmd":"mutate","id":"…"}`, `{"cmd":"pause"}`, `{"cmd":"resume"}`, `{"cmd":"stop"}`,
  `{"cmd":"start-service","name":"postgres"}`.
- `GET /api/scenarios` → `{ services: { postgres }, current, state: idle|running|finished, scenarios: [{ id, scenario }] }`;
  `GET /api/services` → `{ postgres }`.

## Dashboard

`web/torture/` — plain HTML, CSS custom properties and custom elements that
clone `<template>`s into shadow roots; canvas charts; no dependencies or build
step. Design tokens and rules: [docs/design-system.md](../../docs/design-system.md).
