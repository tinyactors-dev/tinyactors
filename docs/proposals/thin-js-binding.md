# A thin JavaScript binding over a deep native one

**Status:** Sketch (2026-10-10).
**Touches:** tinyactors-core: `bindings/shared/core.mjs` (rewritten),
`bindings/shared/binding.c` (narrowed and deepened),
`bindings/shared/ecmascript.mjs`, `bindings/node`, `bindings/web`,
`bindings/surface.json`, `examples/`; `src/system` (an error for a failed
external invoke), `src/host` (run loop policy, containers), `src/trace`
(rule parsing by name).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Summary

`bindings/shared/core.mjs` is 3555 lines. About a fifth of it emulates
xstate on top of SCXML, an eighth duplicates C that already exists, and
another eighth is logic free of any JavaScript API that belongs in C. Its
hot path is expensive: one `actor.send` costs three native calls, about
twenty Node-API calls per guard or action, a call per link, and a rebuilt
snapshot plus a `JSON.stringify` for *every* observed actor, stepped or
not. On the web build each Node-API call is a wasm→JS crossing. And the
0.2 API lost the two extension points SCXML gives a host, Event I/O
Processors and invoke types, which the reference application (granary)
uses through the 0.1 API.

The proposal:

1. **The core and SCXML semantics are the source of truth.** A JS name
   is SCXML's or the core's name with the same meaning. xstate lends a
   name only where its meaning is identical; where it differs, we use
   SCXML's name and meaning and never emulate xstate. A chart is SCXML,
   written as XML or as a plain config object that compiles element by
   element to SCXML. Expressions are ECMAScript text, and what a chart
   needs from the application are functions it calls by name, as SCXML
   text would call any function. The SCXML is fully readable and is the
   serialization format. The examples show what the system can do; they
   do not decide the API.
2. **Expose SCXML's extension points:** I/O processors for `<send type>`
   and invokers for `<invoke type>`, registered on the system, reporting
   back only through what the core already defines.
3. **Put the deep layer in C, inside the binding.** `binding.c` gets a
   `System` that owns the host block, the datamodel, the pump, the timer,
   and notification of actors that stepped. JavaScript does not drive the
   run loop.
4. **Move what is free of JavaScript further down,** into `src/host` and
   `src/trace`, and bind `address.h` and `frame.h` instead of keeping JS
   twins.
5. **Keep JavaScript for what Node-API is bad at:** promises and streams
   (stores, transports, I/O processors and invokers, the OTLP exporter),
   plus the ECMAScript evaluator.

## 1. Whom it serves, who pays

- **Application developer, first.** Asks: what does my chart do? Why did
  this condition fail? How do I reach the outside world from a chart? Why
  is a send slow with many subscribers? Gets: one chart format, SCXML,
  readable in full in `toSCXML()`, in traces and in errors, meaning what
  the SCXML specification says; application functions with closures and
  imports, called by name from the chart; I/O processors and invokers for
  side effects; a send whose cost does not grow with the number of observed
  actors. Loses: xstate's config language and its snapshot (section 4).
- **Granary, the reference application** (`docs/roles.md`), gets back the
  extension points it uses through the 0.1 API: `IOProcessor` (the GitHub
  outbox processor, `src/lib/server/io/github.ts:42`; the catch-up
  processor, `src/lib/server/github/catchup.ts:106`) and `Invoker`
  (`src/lib/ops/feature.ts:65`). It can then move to the current runtime.
- **Binding author.** Asks: what must I write myself? Today: read 3555
  lines of JavaScript and port what is not about JavaScript. After: the
  run loop policy, containers and trace names are C; a binding writes the
  datamodel's value handling, the codec, timers, I/O processors and
  invokers in its language's terms.
- **Maintainers.** One implementation each of addresses, frames, the run
  loop and trace presets. One datamodel vtable instead of two. A JS layer
  readable in one sitting.
- **Capacity planner.** The JS binding's per-event cost becomes a measured
  number (section 9).

**Who pays.**

- *C embedder:* nothing per event or per actor. The core change (section
  7) widens an existing inbound flag. New C in `src/host` is policy the
  binding used to own; an embedder that drives `ta_system` directly does
  not link it.
- *Maintainers:* a switchover commit that replaces `core.mjs` and migrates
  every test and both examples, and a few hundred lines of C with tests.
- *Application code:* the examples are rewritten as SCXML-shaped configs.

**Out of scope.** Replacing `ecmascript.mjs` with quickjs. The link
protocol stays in JavaScript (section 8). Dynamic children (xstate's
`spawnChild`), which would need a core feature.

## 2. What is there today

| Kind | LOC | Examples |
|---|---|---|
| xstate emulation | ~700 | `Compiler` (251–629); a tokenizer that splits function *source text* so each `assign` property becomes an `<assign>` (117–249); target resolution; `after` lowering; `stateValue`/`flattenValue`; `linkMachine`, link status as a compiled xstate machine |
| Duplicates C | ~450 | addresses (1034–1089, "the JS twin of address.h"); `FrameDecoder` (3412–3498, `frame.h`); wire and base64 (947–1009, also `WIRE_JS` in `datamodel_ecmascript.c`); trace kind order and presets (1091–1207, `trace.h`); segment and version maps mirroring `ta_host_chart` (2208–2297); `REASONS` (`ta_host_route_reason`) |
| Free of JS, not in C | ~450 | link hello/caps/reach/resume (2803–3168); snapshot and export containers (1751–1857, 1970–2049); detail-class allocation; invoke `src` lookup |
| Binding work | ~1900 | `System` glue, hibernation I/O, OTLP exporter, store link, tagged values, transports |

`binding.c` carries the runtime half of the emulation: a second datamodel
vtable that renames `done.invoke.*` to `xstate.done.actor.*`, calls
`Object.assign` through Node-API once per assigned property, allocates an
`{context, event, input}` and a `$` object per evaluation, and parses the
session serial back out of the `sessionid` string.

The binding reads `_ioprocessors` (`binding.c:1406`) but offers no way to
register an I/O processor, and it does not handle `TA_EFFECT_INVOKE` or
`TA_EFFECT_UNINVOKE` at all.

## 3. Charts

A chart is SCXML. The config object is SCXML's elements and attributes
under their own names, so it compiles one to one through
`ta_xml_writer_*` (`native.writeXML`), and `fromSCXML` reads the same
thing as XML.

| Config | SCXML |
|---|---|
| `createMachine({ name, initial, datamodel, states })` | `<scxml name initial datamodel="ecmascript">` |
| `datamodel: { opens: 0 }` | `<datamodel><data id="opens" expr="0"/>`; values are JSON |
| `states: { closed: { … } }` | `<state id="closed">`; the key is the state's id and must be unique in the chart |
| `type: 'parallel' \| 'final' \| 'history'`, `history: 'deep' \| 'shallow'` | `<parallel>`, `<final>`, `<history type>` |
| `initial` | `initial` attribute |
| `transitions: [{ event, cond, target, type, actions }]` | `<transition>`s in document order; `event` is an SCXML event descriptor list (prefix matching, `*`); no `event` is an eventless transition; `type: 'internal'` as in SCXML |
| `onentry`, `onexit` | `<onentry>`, `<onexit>` |
| `invoke: { id, type, src, params, autoforward, finalize }` | `<invoke>` with `<param>`s; `src` names a chart in the system (resolved by `ta_image_chart_by_name`) or, with a `type`, a service for that type's invoker (section 5) |
| `donedata: { params } \| { content }` on a final state | `<donedata>` |

`cond`, `expr` and `params` values are ECMAScript expressions, as text.

Executable content (`actions`, `onentry`, `onexit`, `finalize`) is a list
whose entries are SCXML's elements:

| Entry | SCXML |
|---|---|
| a string | `<script>` with that text |
| `raise(event)` | `<raise event>` (no data, as in SCXML) |
| `send(event, { target, type, id, delay, namelist, params, content })` | `<send>`; without `target` it goes to the session's external queue; `delay` is a duration (`'5s'`, `'200ms'`) |
| `cancel(sendid)` | `<cancel sendid>` |
| `assign(location, expr)` | `<assign location expr>` |
| `log(label, expr)` | `<log label expr>`; `expr` is an expression |

There is no `if` and no `foreach` entry: a branch or a loop belongs in a
function the chart calls.

### Functions the chart calls

`setup({ functions })` gives charts the application's functions. They
are installed in every session's scope of the charts made by that
`setup`, read-only, like `In` (`ecmascript.mjs:432`), and a chart calls
them as ECMAScript calls them: `cond="canOpen()"`, `<script>track("door")</script>`.

- A function is called with the session's scope as `this`, which is what
  ECMAScript does for any function called through a `with` scope, and what
  `ecmascript.mjs` does for functions a `<script>` declares. `this.opens`
  reads or writes a datamodel variable, and `this._event` is the current
  event. Arguments are exactly the call's.
- Arrow functions have no `this` of their own; write methods or `function`
  declarations (open question 5).
- A function whose name is a `<data>` id or a system variable (`_event`,
  `_sessionid`, `_name`, `_ioprocessors`, `In`) is an error when the chart
  is attached.
- Functions are not datamodel state: hibernation, export and snapshots
  leave them out, as they leave out `In`, and a restored session gets them
  again from its chart's `setup`.

`setup({ functions }).fromSCXML(xml)` gives hand-written SCXML the same
functions. A config chart and an XML chart are the same thing at run time.

### The door, as a config and as SCXML

```js
import { setup, send, cancel } from '@tinyactors/node';

const door = setup({
  functions: {
    canOpen() { return !this.locked; },
    track(tag) { analytics.track(tag); },
  },
}).createMachine({
  name: 'door',
  initial: 'closed',
  datamodel: { opens: 0, locked: false },
  states: {
    closed: {
      transitions: [{ event: 'open', cond: 'canOpen()', target: 'opened',
                      actions: ['opens += 1', 'track("door")'] }],
    },
    opened: {
      onentry: [send('close', { delay: '5s', id: 'autoClose' })],
      onexit: [cancel('autoClose')],
      transitions: [{ event: 'close', target: 'closed' }],
    },
  },
});
```

```xml
<scxml name="door" initial="closed" datamodel="ecmascript">
  <datamodel><data id="opens" expr="0"/><data id="locked" expr="false"/></datamodel>
  <state id="closed">
    <transition event="open" cond="canOpen()" target="opened">
      <script>opens += 1</script>
      <script>track("door")</script>
    </transition>
  </state>
  <state id="opened">
    <onentry><send event="close" delay="5s" id="autoClose"/></onentry>
    <onexit><cancel sendid="autoClose"/></onexit>
    <transition event="close" target="closed"/>
  </state>
</scxml>
```

## 4. Actors and systems

Core vocabulary, with SCXML's names for what SCXML defines.

| JS | Core and SCXML |
|---|---|
| `createSystem({ name, clock, ioProcessors, invokers, hibernation, restore, … })` | `ta_host_new` (`system_id`, `io_processors` in `ta_host_config`) |
| `system.spawn(chart, { name, params })` → `Actor` | `ta_host_spawn`: the actor starts at once. `params` overwrite the `<data>` of the same id, as invoke params do. `name` makes it a named actor (`@name`). |
| `system.get(name)` | `ta_host_actor_by_name` |
| `actor.send(event, data)` | `ta_host_send`; the session sees `_event` as SCXML defines it (`name`, `type`, `sendid`, `origin`, `origintype`, `invokeid`, `data`) |
| `actor.terminate()` | `ta_host_terminate`: the actor exits at its next stable point, running its `<onexit>` handlers |
| `actor.configuration` | the active state ids (`ta_actor_config`) |
| `actor.in(id)` | `In(id)` |
| `actor.datamodel` | the session's variables |
| `actor.done` | a promise: resolved with the donedata on `TA_EFFECT_DONE`, rejected on `TA_EFFECT_ERROR` (a fault; the actor was reclaimed). Root actors only, as in the core. |
| `actor.subscribe(fn)` | called once after each slice in which the actor took a step; several steps of one slice are seen once |
| `actor.sessionID`, `actor.name`, `actor.address` | `_sessionid`, `_name`, its `tinyactors://` address |
| `actor.export()` → blob | `ta_host_export_actor`: the actor **leaves** the system |
| `system.adopt(blob)` | `ta_host_adopt`: refused while the blob's session exists |
| `system.snapshot()`, `createSystem({ restore })` | `ta_host_snapshot` / `ta_host_restore`, the whole host |
| `system.hibernate()`, `hibernation: { store }` | `ta_host_sweep`, `ta_host_hibernate`, `ta_host_wake` |
| `system.reload(chart)`, `system.migrate(…)` | `ta_host_add_image`, `ta_host_migrate` |
| `system.flush()` | advance the logical clock and run to idle (with a `clock` option) |

Event names are the core's: `done.invoke.<id>`, `done.state.<id>`,
`error.execution`, `error.communication`, `error.platform.upgrade`. The
binding adds none and renames none.

## 5. I/O processors and invokers

SCXML gives a host two extension points; the core implements both and
reports everything back through inbound flags it already has.

```js
createSystem({
  ioProcessors: {
    'https://granary.dev/io/github': {
      location: 'github',
      send(event, ctx) { … },
    },
  },
  invokers: {
    'https://granary.dev/invoke/fetch': fromPromise(async ({ src, params, signal }) => { … }),
    'https://granary.dev/invoke/watch': fromCallback(({ src, params, sendBack, receive }) => { … }),
  },
});
```

**I/O processors** (`<send type="…">`). Registered at creation, because
`ta_host_config.io_processors` is fixed for the host's life; each appears
in `_ioprocessors` with its `location`.

| Processor | Core |
|---|---|
| `send(event, ctx)`: `event` is `{ target, name, sendid, data, origin }` | a `TA_EFFECT_SEND` whose `io_processor` is this processor's index |
| it throws, or its promise rejects | `TA_EVENT_IN_ERROR_COMMUNICATION` with the `sendid`: `error.communication` in the sender's internal queue (SCXML §6.2) |
| `ctx.send(address, name, data)` | an event into the system (`ta_host_send`, or the router for another system's address) |

**Invokers** (`<invoke type="…">`). The `type` is a URI the application
chooses, as SCXML §6.4 allows; the core turns any `<invoke>` with a
non-SCXML type into an external invoke (`src/system/route.c:405`).

| Invoker | Core |
|---|---|
| started with `{ id, src, params }` | `TA_EFFECT_INVOKE` (`event` = type, `target` = src, `sendid` = invokeid, `payload` = params) |
| `sendBack(name, data)` | `ta_event_in` with `TA_EVENT_IN_FROM_INVOKE`; the core sets `_event.invokeid` (`schedule.c:392`) |
| `receive(fn)`: events the chart sends to `#_<invokeid>` | `TA_EFFECT_SEND` carrying the invoke token |
| the service finishes with a result | `TA_EVENT_IN_INVOKE_DONE`: `done.invoke.<id>` with the result as data |
| the service fails (throws, rejects) | `TA_EVENT_IN_ERROR_COMMUNICATION` with the invoke token: `error.communication` in the invoker's internal queue, with `_event.invokeid` set; SCXML §3.12.2 names errors arising from `<invoke>` communication errors. Needs the core change in section 7. |
| the invoking state exits | `TA_EFFECT_UNINVOKE`: `signal` aborts, the cleanup runs |

`fromPromise` and `fromCallback` are two small helpers that make an
invoker from a function: a promise resolves to `INVOKE_DONE` and rejects to
the error; a callback service sends any number of events, may finish, and
returns its cleanup. A chart with an external invoke in flight is not
hibernated, swept or exported: the core refuses it already
(`hibernate.c:140–150`, `system.h:404–405`).

## 6. What does not map, and goes

| Today (xstate) | Why |
|---|---|
| inline functions in a config | they forced the source tokenizer. Charts call named functions (section 3). |
| `assign` as an immutable partial merge, actions returning context | SCXML's `<assign location expr>` writes one location; a `<script>` discards its value. Functions write `this.<variable>`. |
| `context`, `input` | SCXML's `datamodel` (mutable variables) and `params` |
| `guards`, `actions`, `actors`, `delays` in `setup` | a chart calls functions; SCXML has no separate kinds. `src` names a chart directly. Delays are durations. |
| `on: { event: … }`, `always`, `onDone`, `onError` | `transitions`, an ordered list: SCXML matches descriptors by prefix in document order, and a done or error event is a transition on its name like any other |
| relative targets (`.child`, siblings) | state keys are SCXML ids |
| `reenter` | SCXML's `type: 'internal'` |
| `after` | `send(e, { delay, id })` in `onentry`, `cancel(id)` in `onexit` |
| `raise(e, { delay })`, `raise` with data | `<raise>` takes neither: use `send` (with `target: '#_internal'` for the internal queue) |
| `sendTo`, `sendParent`, `stateIn`, `and`/`or`/`not` | `send` with a `target` (`'#_parent'`), `In()` and boolean expressions in `cond` text |
| `snapshot.value`, `.matches`, `.status`, `.context`, `.output`, `getSnapshot` | `configuration`, `in()`, `datamodel`, `done` (section 4); xstate's status values do not exist in the core |
| `getPersistedSnapshot`, `createActor(logic, { snapshot })` | they read without removing; the core's export removes the actor. `export` and `adopt` say what happens. |
| `createActor` + `start`, `stop`, the system ID option | `system.spawn` (starts at once), `terminate`, `name` |
| events as `{ type, ...data }` | `_event.type` is SCXML's event type; events are a name and data |
| `tags`, `hasTag`, `meta`, `description`, `can`, `inspect`, `toPromise`, `waitFor` | no core or SCXML meaning; `actor.done` and `subscribe` cover the last two; traces cover `inspect` |
| `spawnChild`, `stopChild`, `enqueueActions`, `emit`, `fromObservable`, `fromTransition` | no SCXML equivalent |
| `provide`, the implicit default system, the global machine registry, `linkMachine` | emulation or global state |

## 7. The core change

`TA_EVENT_IN_ERROR_COMMUNICATION` reports a failed `TA_EFFECT_SEND`: it
puts `error.communication` with the `sendid` in the target's internal
queue (`schedule.c:358`), and ignores the token. Widen it to external
invokes: with an invoke token, also set `_event.invokeid`, as
`TA_EVENT_IN_FROM_INVOKE` does (`schedule.c:383–393`). No new flag, no new
event name; a C embedder running its own invokers gets it too.

## 8. What moves into C

Each item deletes its JS twin in the same change.

1. **Addresses and frames.** Bind `address.h` and `frame.h` (`skip` in
   `surface.json` today).
2. **Run loop policy.** Retrying while `EFFECTS_FULL`, draining traces
   into the encoder before effects, `root_session`, job state:
   `ta_host_pump(host, now, budget, out)`, usable by every binding.
3. **Containers.** System snapshot and actor export records become a
   C-defined layout around an opaque datamodel section. `adopt` finds a
   segment by `ta_image_get_info.content_hash`, not by re-encoding JSON.
4. **Image lookups.** Invoke `src` → chart and the current version per
   chart name come from `ta_host_chart`; the segment and version maps in
   JS go.
5. **Trace names.** Rule and detail parsing by name, presets, class
   allocation: the binding passes the user's object as JSON text.

The link protocol (hello, capabilities, reach, resumption, ~325 lines) is
also free of JavaScript, but it is the largest piece and only JavaScript
uses it today. It stays in JS, rewritten over the C address and frame code,
and without `linkMachine`.

## 9. The native seam and the JS layer

`binding.c` stays procedural: a `System` is a native handle, and a JS
class of about 40 lines forwards its methods to `native.*(handle, …)`.
That class is what `index.d.ts` and the `surface` check see. A Node-API
class would not make the seam deeper, and it would grow the web build's
Node-API subset (`napi_define_class`, wrapping) for no gain.

**The native `System`** owns the host block and grows it on `EXHAUSTED`;
holds the one datamodel vtable (over `ecmascript.mjs`) and the codec; runs
the pump in budgeted slices on a platform `defer` and one platform timer
for `next_deadline`, both passed in at creation; and after each slice
calls the observers of actors that stepped, once each. Effects for JS
(sends to registered I/O processors, invokes and uninvokes, logs, done,
errors, peer envelopes) go to callbacks given at creation, as one call
each. An evaluation is one call into `ecmascript.mjs`; `_event` is built
once per `bind_event`.

**Pure functions,** about fifteen, written by hand: compile, image info,
addresses, traceparent, span IDs, decoding traces and metrics, frames.

**What remains in `core.mjs`:**

- `setup`, `createMachine` (config → SCXML), the executable content
  entries: about 200 lines;
- `createSystem`, `Actor` over the native `System`: about 150;
- I/O processors, invokers, `fromPromise`, `fromCallback`: about 100;
- stores and hibernation I/O, the tagged value codec: about 200;
- links and the store link over transports: about 300;
- the OTLP exporter and telemetry options: about 150.

About 1100 lines, plus `ecmascript.mjs`.

**Measuring it.** Before the switchover, add a JS scenario reporting
throughput and memory together (in `tools/bench_js.mjs` or the torture
chamber): sends per second and bytes per actor with 0, 10 and 1000
observed actors, on Node and the web build. Measure today's binding, then
the new one, side by side on one machine.

## 10. Decided

- **Keep `bindings/shared`.** One `binding.c` compiled as the Node-API
  addon and as the wasm module, one `core.mjs` over both. Two bindings,
  one per target, would duplicate the datamodel, the payload tables and
  the pump.
- **No wasm everywhere.** Node keeps the native addon.
- **A procedural seam behind a small JS class** (section 9).
- **Hand-written bindings.** Generating them from `surface.json` was
  considered: it would need parameter annotations the types do not carry
  (output arrays, grow-and-retry buffers, owned blocks) and would generate
  only the shallow part of the API, while a 1:1 binding of `system.h` and
  `host.h` would put memory management and the run loop back in
  JavaScript. The JS layer calls about fifteen pure functions directly;
  writing them is less code than the generator. `surface.json` stays the
  checklist of what the bindings reach.
- **Errors are SCXML's.** `error.execution` for errors in the chart,
  `error.communication` for a failed send or invoke, through the core's
  existing effect-error path. No per-invoke error transitions.

## 11. Open questions

1. **An invoke after its error.** Does the external invoke stay
   registered after `error.communication` until the state exits and
   `UNINVOKE` arrives, or should the error end it? SCXML only says the
   error goes on the queue.
2. **A faulted invoked chart.** The invoker gets no event today
   (`schedule.c:205`). Whether it should get `error.communication` is a
   core decision.
3. **Actors that ended.** What the binding can still read after
   `TA_EFFECT_DONE`, a fault or `terminate` (actor info, configuration),
   and whether terminating a root actor produces an effect `actor.done`
   can settle on.
4. **`TA_IO_PROCESSOR_SYNC`.** Whether JS I/O processors need it: the run
   returns after such a send so the outcome lands in the sender's
   macrostep. A JS processor would have to answer synchronously.
5. **`this` or an argument.** Functions see the scope as `this`, which
   arrow functions cannot. The alternative passes the scope as the first
   argument; that is a calling convention of our own, while `this` is
   ECMAScript's.
6. **The link protocol in C,** later, if a second binding needs links.

## 12. Order of work

Each step is a commit that passes `mise run test`.

1. Bind `address.h` and `frame.h`; delete the JS twins.
2. `ta_host_pump`; `binding.c`'s `run` calls it.
3. Containers and adoption by content hash in C.
4. Trace policy by name in C.
5. `TA_EVENT_IN_ERROR_COMMUNICATION` for invoke tokens, with a C test.
6. The benchmark scenario, measured on today's binding.
7. The switchover: the native `System`, one datamodel, I/O processors and
   invokers in `binding.c`; the new `core.mjs`; tests and examples
   migrated; the dropped API deleted; `surface.json` and both
   `index.d.ts` updated. `mise run test:full`.
8. Measure again; record both in the commit message.

Steps 1–5 shrink today's `core.mjs` before it is replaced, so the
switchover is binding work only.
