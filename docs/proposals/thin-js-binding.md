# A thin JavaScript binding over a deep native one

**Status:** Sketch (2026-10-10).
**Touches:** tinyactors-core: `bindings/shared/core.mjs` (rewritten),
`bindings/shared/binding.c` (narrowed and deepened),
`bindings/shared/ecmascript.mjs` (removed), `bindings/node`,
`bindings/web`, `bindings/surface.json`, `examples/`, `tests/w3c`;
`src/system` (failed external invokes, snapshots with external invokes,
actors that stepped), `src/host` (run loop
policy, containers), `src/trace` (rule parsing by name).
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

1. **The core and SCXML semantics are the source of truth.** Every name
   in the JS API is SCXML's or the core's, with the same meaning, or a
   plain name for a composite the proposal defines. Nothing is emulated,
   and the binding invents no events, fields or mechanisms.
2. **A system chooses its ECMAScript datamodel:** a native one whose
   values are JS values and whose expressions are JS functions, for
   trusted application code at full speed, or quickjs for untrusted code;
   staticjs comes later (section 3).
3. **Charts are SCXML, written as XML or as JS objects,** because
   JavaScript developers do not read XML. Concise helpers build the
   objects, which go one way: object → writer → SCXML text → compiler.
   `c.expr(fn)` marks code (section 4).
4. **Expose SCXML's extension points:** I/O processors for `<send type>`
   and invokers for `<invoke type>`, registered on the system (section 6).
5. **Put the deep layer in C, inside the binding,** and move what is free
   of JavaScript further down (sections 8 and 9).
6. **Keep JavaScript for what Node-API is bad at:** promises and streams
   (stores, transports, I/O processors, invokers, the OTLP exporter).

## 1. Whom it serves, who pays

- **Application developer, first.** Asks: what does my chart do? Why did
  this condition fail? How does my chart reach the outside world? Why is
  a send slow with many subscribers? Gets: charts in JS whose code is real
  JS (type-checked, closures and imports work) when the chart is trusted;
  a sandbox when it is not; the outside world through I/O processors and
  invokers; a send whose cost does not grow with the number of observed
  actors. Loses: xstate's config language and snapshot (section 7).
- **Granary, the reference application** (`docs/roles.md`), gets back the
  extension point it builds on: its I/O processors (the GitHub outbox,
  `src/lib/server/io/github.ts:42`; catch-up,
  `src/lib/server/github/catchup.ts:106`; the processors in
  `src/lib/ops/io/`). They reply to the sender, use an abort signal and
  carry the trace context, which section 6 provides. It types an `Invoker`
  field (`src/lib/ops/feature.ts:65`) but registers none.
- **Binding author.** Asks: what must I write myself? After this: a native
  datamodel for the language's own values, the codec, timers, and the I/O
  processor and invoker interfaces. The run loop policy, containers and
  trace names are C.
- **Maintainers.** One implementation each of addresses, frames, the run
  loop and trace presets. No ECMAScript evaluator in JavaScript beside the
  C one. A JS layer readable in one sitting.
- **Capacity planner.** The JS binding's per-event cost becomes a measured
  number (section 10).

**Who pays.**

- *C embedder:* the core changes (section 8) cost nothing per event when
  unused, except recording which actors stepped: one bounded list per
  run, an append per macrostep. A long-lived external invoke blocks host
  snapshots for as long as it runs.
- *Maintainers:* a few hundred lines of C with tests; a switchover of
  `core.mjs` in several commits; every test and both examples migrated.
- *Application code:* the examples are rewritten with the helpers.

**Out of scope.** The link protocol stays in JavaScript (section 9).
Dynamic children, and invoking a chart of another segment (`src` resolves
within the invoker's image), which would both need core features.

## 2. What is there today

| Kind | LOC | Examples |
|---|---|---|
| xstate emulation | ~700 | `Compiler` (251–629); a tokenizer that splits function *source text* so each `assign` property becomes an `<assign>` (117–249); target resolution; `after` lowering; `stateValue`/`flattenValue`; `linkMachine`, link status as a compiled xstate machine |
| Duplicates C | ~450 | addresses (1034–1089, "the JS twin of address.h"); `FrameDecoder` (3412–3498, `frame.h`); wire and base64 (947–1009, also `WIRE_JS` in `datamodel_ecmascript.c`); trace kind order and presets (1091–1207, `trace.h`); segment and version maps mirroring `ta_host_chart` (2208–2297); `REASONS` (`ta_host_route_reason`) |
| Free of JS, not in C | ~450 | link hello/caps/reach/resume (2803–3168); snapshot and export containers (1751–1857, 1970–2049); detail-class allocation; invoke `src` lookup |
| Binding work | ~1900 | `System` glue, hibernation I/O, OTLP exporter, store link (3170–3410), tagged values, transports |

`binding.c` holds two datamodel vtables. The "xstate" one keeps payloads
as Node-API references and calls one JS function per expression, which is
the right shape, but renames `done.invoke.*` to `xstate.done.actor.*`,
calls `Object.assign` through Node-API once per assigned property,
allocates an `{context, event, input}` and a `$` object per evaluation,
and parses the session serial out of the `sessionid` string. The
"ecmascript" one forwards to `ecmascript.mjs`, an ECMAScript evaluator in
JavaScript (`new Function` in the application's realm). The binding reads
`_ioprocessors` (`binding.c:1406`) but cannot register an I/O processor,
and does not handle `TA_EFFECT_INVOKE` or `TA_EFFECT_UNINVOKE`.

## 3. Datamodels

SCXML leaves the set of datamodels to the platform (the `datamodel`
attribute); without it the compiler uses the null datamodel
(`scxml.c:1020`). The core gives each chart the first datamodel of its
system whose name matches (`system.c:221–226`), so a system chooses one
implementation of `"ecmascript"`:

| `createSystem({ ecmascript })` | Values | Expressions | For |
|---|---|---|---|
| `'native'` | JS values held as Node-API references; payloads are references | JS functions, called directly | trusted application code |
| `'quickjs'` | quickjs values (`src/datamodel_ecmascript`, in the addon and in the wasm module, or a quickjs-wasi build) | ECMAScript text | untrusted code; the W3C suite |

staticjs comes later as a datamodel of its own name.

**The native implementation** is the "xstate" vtable with the xstate
parts removed. A function is called with one argument: an object holding
the session's variables and `_event`, `_sessionid`, `_name`,
`_ioprocessors` and `In`. An expression's value is the function's result;
a `<script>` is a function called for its effects; `<assign location
expr>` writes the location with the expression's value. Events keep their
core names. Functions are closures: they can read module constants and
call application code, because they run in the application's realm. A
chart with functions cannot be serialized as text: `toSCXML()` shows each
function's source for reading only. Hibernation and snapshots store
values through the codec and refer to charts by name and version, as
today.

**`ecmascript.mjs` is removed.** It evaluates text in the application's
realm, so it is neither trusted functions nor a sandbox. The W3C suite
runs under quickjs.

**Values between implementations.** Within a system there is one
implementation of `"ecmascript"`, so payloads never meet a datamodel that
did not make them. Systems of different implementations exchange events
over links, where the core already converts with `to_wire`/`from_wire`
(neutral JSON, `datamodel.h:171–183`).

## 4. Charts in JS

### The chart object

The chart object is SCXML's document tree as JS values:

- **One object per element:** `tag` is the element's qualified name
  (`'state'`, or `'conf:pass'` for a foreign element), `children` its
  child elements in document order. Neither is an SCXML attribute name;
  the element name cannot be a key itself, because `datamodel`, `initial`
  and `content` are both elements and attributes.
- **Attributes are the other keys,** under their qualified names
  (`xmlns:conf` included), with string values; any other value is
  refused. In a native system, an expression attribute may hold
  a `c.expr` function instead.
- **Text** (a `<script>`, a `<content>`, a `<data>`) is `text`; mixed
  content is a `children` list of strings and elements. XML inside a
  `<content>` or `<data>`, and an inline child chart
  (`<invoke><content><scxml>…`), are elements in `children` like any
  other.

The chart object goes one way. The binding writes it through
`ta_xml_writer_*` (no binding writes or escapes XML) and compiles the
text; `fromSCXML(xml)` compiles XML as it is, as the core always has. No
SCXML is read back into objects.

Diagnostics point at lines of the generated XML (`compile.h:86–91`); the
binding records which object each written line came from and reports the
object's path.

### Helpers

One namespace, one function per SCXML element, named after the element
(`s.if` and `s.else` are valid property names, though not importable
names). Each helper takes its element's identifying attributes as
positional strings, then an optional attributes object, then children.
Strings are positional, a plain object is attributes, an object with
`tag` is a child. Every positional also works as a named attribute, so
the expression forms (`eventexpr`, `targetexpr`, …) stay available.

| Helper | Positional |
|---|---|
| `scxml` | `name` |
| `state`, `parallel`, `final`, `history`, `invoke` | `id` |
| `transition` | `event`, `target` (eventless: `null` event) |
| `data`, `param` | `id` / `name`, `expr` |
| `assign` | `location`, `expr` |
| `raise`, `send` | `event` |
| `cancel` | `sendid` |
| `log` | `label`, `expr` |
| `if`, `elseif` | `cond` |
| `foreach` | `array`, `item` |
| `script`, `content` | text |

`s.datamodel({ items: '[]' })` is a map from `<data>` ids to expressions.
`s.chart<Data>()` returns the same helpers typed for a datamodel, so
expressions are type-checked against it.

**`c.expr(fn)` marks code.** `fn` takes the session object and is
destructured the same way in every datamodel:

- in a native system the function itself is the expression;
- under quickjs, `fn`'s body becomes the expression's text
  (`({ attempts }) => attempts + 1` → `attempts + 1`; a block body →
  `(() => { … })()`). Free names other than the destructured ones are not
  checked: a captured constant fails at run time there.

Writes go through `assign`, which means the same in every datamodel; a
function that assigns to a destructured name changes only its local.

```js
import { scxml as s } from '@tinyactors/node';

const c = s.chart<{ items: Item[]; attempts: number; reservation: Reservation | null }>();

export const order = c.scxml('order', { initial: 'reserving', datamodel: 'ecmascript' },
  c.datamodel({ items: c.expr(() => []), attempts: c.expr(() => 0), reservation: c.expr(() => null) }),

  c.state('reserving',
    c.onentry(c.assign('attempts', c.expr(({ attempts }) => attempts + 1))),
    c.invoke('stock', { type: STOCK, src: 'reserve', namelist: 'items' }),
    c.transition('done.invoke.stock', 'paying',
      c.assign('reservation', c.expr(({ _event }) => _event.data))),
    c.transition('error.communication', { cond: c.expr(({ _event }) => _event.invokeid === 'stock') },
      c.if(c.expr(({ attempts }) => attempts < MAX_RETRIES),
        c.send('retry', { delay: '2s' }),
      c.else(),
        c.raise('gave.up'))),
    c.transition('retry', 'reserving'),
    c.transition('gave.up', 'failed'),
  ),

  c.state('paying',
    c.onentry(
      c.foreach('items', 'item', c.log('charging', c.expr(({ item }) => item.sku))),
      c.send('charge', { type: PAYMENTS, target: 'stripe' },
        c.param('reservation', 'reservation'),
        c.param('amount', c.expr(({ items }) => items.reduce((s, i) => s + i.price, 0))))),
    c.transition('payment.ok', 'done'),
    c.transition('payment.declined', 'failed'),
    c.transition('error.communication', 'failed'),
  ),

  c.final('done', c.donedata(c.param('reservation', 'reservation'))),
  c.final('failed'),
);
```

### Images and documents

`chart()` and `fromSCXML()` give one chart. `system.addImage(charts, {
documents })` compiles several into one image (`ta_compile_charts`):
`<invoke src>` resolves within it, and `documents` answers `<data src>`,
`<script src>` and `<invoke src>` at compile time (`scxml.c:202–216`).

### Testing it

The mapping is mechanical, so unit tests cover it: each helper and each
attribute form writes the expected XML, and a chart built with the
helpers behaves as the same chart written as XML.

## 5. Actors and systems

| JS | Core and SCXML |
|---|---|
| `createSystem({ systemID, clock, ioProcessors, invokers, log, hibernation, restore, … })` | `ta_host_new` (`system_id`, `io_processors`); `log` receives `TA_EFFECT_LOG` |
| `system.addImage(charts, { documents })` | `ta_compile_charts`, `ta_host_add_image`; a chart name already loaded gets a new version (hot reload) |
| `system.spawn(chart, { name, params })` → `Actor` | `ta_host_spawn`: the actor starts at once. `params` overwrite the `<data>` of the same id. Every actor has a name: the given one, else the chart's (`system.c:531–536`); names are not unique. |
| `system.get(name)` | `ta_host_actor_by_name`: the live actor with that name in the newest segment with the lowest slot, invoked children included (`system.c:760–769`) |
| `system.send(address, event, { data, sendid, origin, origintype, raw, traceparent })` | one `ta_event_in`, delivered locally (`ta_host_send`) or through the router |
| `actor.send(event, options)` | `system.send(actor.address, event, options)` |
| `actor.terminate()` | `ta_host_terminate`: the actor stops at the next microstep boundary (`core.c:1767–1772`), running its `<onexit>` handlers |
| `actor.configuration`, `actor.in(id)` | `ta_actor_config`; `In(id)` |
| `actor.datamodel` | the session's variables: the live object in a native system, a copy (`to_wire`) under quickjs; `null` while hibernated |
| `actor.done` | for a root actor: resolved with the donedata on `TA_EFFECT_DONE`, rejected on `TA_EFFECT_ERROR` for its session. A terminated actor produces no effect (`system.c:678–679`, `route.c:726`), so the binding settles it when `terminate` succeeds. |
| `actor.observe(fn)` | `fn(actor)` once after each run in which the actor took a step (section 8) |
| `actor.sessionID`, `actor.name`, `actor.address` | `_sessionid`; `_name`, which the core sets to the actor's name (SCXML: the document's `name`; `system.c:474`); `"<base>#_scxml_<id>"` (`system.h:46–47`) |
| `actor.export()` → blob | `ta_host_export_actor`: the actor **leaves** the system |
| `system.adopt(blob)` | `ta_host_adopt`: refused while the blob's session exists |
| `system.snapshot()`, `createSystem({ restore })` | `ta_host_snapshot` / `ta_host_restore` |
| `system.sweep()`, `hibernation: { store }` | `ta_host_sweep`, `ta_host_hibernate`, `ta_host_wake` |
| `system.migrate(…)` | `ta_host_migrate` |
| `system.flush()` | with a `clock` option: `ta_host_advance_time` to the next deadline and run until idle, repeated |

Event names are the core's: `done.invoke.<id>`, `done.state.<id>`,
`error.execution`, `error.communication`, `error.platform.upgrade`. The
binding adds none and renames none.

## 6. I/O processors and invokers

```js
createSystem({
  systemID: 'shop',
  ioProcessors: {
    'https://shop.dev/io/payments': {
      location: 'payments',
      async send({ target, event, sendid, data, origin }, { system, signal, traceparent }) { … },
    },
  },
  invokers: {
    'https://shop.dev/invoke/stock': {
      start({ invokeid, type, src, params }, { send, done, fail, signal, traceparent }) { … },
      receive(invokeid, event) { … },   // optional
    },
  },
});
```

**I/O processors** (`<send type="…">`). Registered at creation:
`ta_host_config.io_processors` is fixed for the host's life (and given
again at restore). There may be 16; the host's own processor comes first
and never reaches JS (`host.c:283`, `:855`). Each appears in
`_ioprocessors` with its `location`, the same for every session
(`system.h:34`).

| Processor | Core |
|---|---|
| `send(message, context)`: `target`, `event`, `sendid`, `data`; `origin`, the sender's address, built from the effect's `session`; `context.traceparent` from the effect's trace context; `context.signal` aborts when the system stops | a `TA_EFFECT_SEND` with this processor's index; a send with a `type` goes to its processor whatever the `target` (`core.c:1005–1007`) |
| it throws, or its promise rejects | `TA_EVENT_IN_ERROR_COMMUNICATION` with the `sendid`: `error.communication` in the sending session's internal queue (SCXML §6.2); a late failure starts a macrostep of its own (`core.c:1847–1852`) |
| a reply: `context.system.send(origin, event, { data, traceparent })` | section 5 |

**Invokers** (`<invoke type="…">`). The `type` is a URI the application
chooses (SCXML §6.4); the core turns any `<invoke>` with a non-SCXML type
into an external invoke (`route.c:405`). `src` is the invoke's `src` or
its `<content>`, which SCXML treats alike (`core.c:1418–1426`).

| Invoker | Core |
|---|---|
| `start({ invokeid, type, src, params }, context)` | `TA_EFFECT_INVOKE`; the binding keys the invoke by segment and token (tokens are per segment) and remembers the invoking session |
| `context.send(event, data)` | `TA_EVENT_IN_FROM_INVOKE`: the core sets `_event.invokeid` (`schedule.c:392`) |
| `receive(invokeid, event)`: sends to `#_<invokeid>`, with the invoke's `type` or without one, and autoforwarded events | with the type: a `TA_EFFECT_SEND` for the I/O processor of that type, with the target `#_<invokeid>` (`core.c:965–1007`); without: a `TA_EFFECT_SEND` with the invoke token (`route.c:118–121`, `621–626`) |
| `context.done(data)` | `TA_EVENT_IN_INVOKE_DONE`: `done.invoke.<id>` with `data`. The binding then closes the context: SCXML §6.4 forbids events after done, and the core frees the request only at uninvoke (`route.c:585–596`). |
| `context.fail()`, or `start` or `receive` throws, or a promise rejects | `TA_EVENT_IN_ERROR_COMMUNICATION \| TA_EVENT_IN_FROM_INVOKE`: `error.communication` in the invoking session's internal queue, with `_event.invokeid` (SCXML §3.12.2). The binding closes the context. Section 8. |
| the invoking state exits | `TA_EFFECT_UNINVOKE`: `signal` aborts; later calls are dropped (SCXML §6.4.3) |

SCXML §6.4 (`SCXML.md:2096`) sends to an invoked service with the
`type` of its `<invoke>` and the target `#_<invokeid>`: an invoke type is
an I/O processor type too. So the binding registers every invoker's type
as an I/O processor as well, and routes that processor's sends to the
invoker's `receive`.

An actor with an external invoke in flight is not hibernated, swept or
exported; the core refuses it (`hibernate.c:136–150`,
`system.h:404–405`). Snapshots are refused too (section 8).

## 7. What goes

| Today | Why |
|---|---|
| `createMachine`, `setup` and xstate's config (`context`, `input`, `on`, `always`, `after`, `entry`/`exit`, `onDone`, `onError`, `reenter`, relative targets) | charts are SCXML, built with the helpers (section 4) |
| named guards and actions in `setup` | a native system's expressions are functions; quickjs charts reach the application through I/O processors and invokers |
| xstate's `assign` merge, `raise` with data or delay, `sendTo`, `sendParent`, `stateIn`, `and`/`or`/`not` | SCXML's `<assign>`, `<raise>`, `<send>` (with `target: '#_parent'`), `In()` and expressions |
| `getSnapshot`, `.value`, `.matches`, `.status`, `.context`, `.output`, `.hasTag`, `.can`, `subscribe` | `configuration`, `in()`, `datamodel`, `done`, `observe` (section 5) |
| `getPersistedSnapshot`, `createActor(logic, { snapshot })` | they read without removing; export removes the actor |
| `createActor` + `start`, `stop`, xstate's system ID option, `system.hibernate()`, `system.reload()` | `system.spawn`, `terminate`, `name`, `sweep`, `addImage` |
| events as `{ type, ...data }` | `_event.type` is SCXML's event type; an event is a name and data |
| `fromPromise`, `fromCallback`, `sendBack` | an invoker (section 6) |
| `ecmascript.mjs` | the native implementation and quickjs cover trusted and untrusted code |
| `tags`, `meta`, `description`, `inspect`, `toPromise`, `waitFor`, `provide`, the default system, the global machine registry, `linkMachine` | no core or SCXML meaning, or global state |

## 8. Core changes

1. **A failed external invoke.** `TA_EVENT_IN_ERROR_COMMUNICATION` with
   `TA_EVENT_IN_FROM_INVOKE`: the system layer checks the token as for
   other invoke events (`schedule.c:383–390`, before the hibernated branch
   at `:342`), refuses a stale one, and sets `invokeid` on the event
   `ta_core_raise_platform` returns (`core.c:2069`); `src/core` is
   unchanged. The flag alone keeps its meaning: a failed send to
   `#_<invokeid>` carries the invoke token too (`system.h:211–212`).
2. **Snapshots with external invokes in flight** are refused with
   `TA_ERROR_STATE`, in `ta_system_snapshot` (`system.c:962–966`) and so
   in `ta_host_snapshot` (`host.c:1733–1735`): a restored system would
   hold invokes no service answers.
3. **Actors that stepped.** `ta_host_run` reports the sessions that took
   a macrostep, in a bounded list (an append per macrostep; past the
   bound, "all"). Neither `ta_actor_info` nor the run result says this
   today, and a datamodel hook would miss bootstrap and terminate steps.
## 9. What moves into C

Each item deletes its JS twin in the same change.

1. **Addresses and frames.** Bind `address.h` and `frame.h` (`skip` in
   `surface.json` today).
2. **Run loop policy.** Retrying while `EFFECTS_FULL`, draining traces
   into the encoder before effects, `root_session`, job state: a new
   `ta_host_pump(host, now, budget, out)`.
3. **Containers.** System snapshot and actor export records become a
   C-defined layout around an opaque datamodel section. `adopt` finds a
   segment by `ta_image_get_info.content_hash`.
4. **Image lookups.** The current version per chart name comes from
   `ta_host_chart`; the segment and version maps in JS go.
5. **Trace names.** Rule and detail parsing by name, presets, class
   allocation: the binding passes the user's object as JSON text.

The link protocol (hello, capabilities, reach, resumption) is free of
JavaScript too, but only JavaScript uses it today. It stays in JS,
rewritten over the C address and frame code, without `linkMachine`.

## 10. The native seam and the JS layer

`binding.c` stays procedural: a `System` is a native handle, and a JS
class of about 40 lines forwards its methods to `native.*(handle, …)`.
That class is what `index.d.ts` and the `surface` check see.

**The native `System`** owns the host block and grows it on `EXHAUSTED`;
holds its ECMAScript implementation (native or quickjs) and the codec; runs the pump in
budgeted slices on a platform `defer` and one platform timer for
`next_deadline`; and after each run calls the observers of the actors
that stepped. Effects for JS (sends to I/O processors, invokes and
uninvokes, logs, done, errors, peer envelopes) go to callbacks given at
creation, one call each. In a native system an evaluation is one
Node-API call with one argument, the session object, built once per
session and updated per `bind_event`.

**Pure functions,** about fifteen, written by hand: compile, image
info, addresses, traceparent, span IDs, decoding traces and metrics,
frames.

**What remains in `core.mjs`:**

| Part | Lines |
|---|---|
| the chart object: objects → writer calls, line → path for diagnostics | ~100 |
| the helpers, `c.expr`, from one element table that also generates the TypeScript types | ~100 |
| `createSystem`, `Actor` over the native `System` | ~150 |
| I/O processors and invokers | ~150 |
| stores and hibernation I/O, the tagged value codec | ~200 |
| links (today ~365 lines) and the store link (~240), over the C address and frame code | ~500 |
| the OTLP exporter and telemetry options | ~150 |

About 1350 lines, and no `ecmascript.mjs`.

**Measuring it.** Before the switchover, add a JS scenario reporting
throughput and memory together (in `tools/bench_js.mjs` or the torture
chamber): sends per second and bytes per actor with 0, 10 and 1000
observed actors, native and quickjs, on Node and the web build. Measure today's
binding, then the new one, side by side on one machine.

## 11. Decided

- **Keep `bindings/shared`.** One `binding.c` compiled as the Node-API
  addon and as the wasm module, one `core.mjs` over both.
- **No wasm everywhere.** Node keeps the native addon.
- **A procedural seam behind a small JS class.**
- **Hand-written bindings.** Generating them from `surface.json` would
  need parameter annotations the types do not carry and would generate
  only the shallow part of the API. `surface.json` stays the checklist of
  what the bindings reach.
- **Charts are SCXML,** as XML or as JS objects built with the helpers.
- **A system chooses its `"ecmascript"` implementation:** native for
  trusted code, quickjs for untrusted code; staticjs later.
- **`ecmascript.mjs` is removed.**
- **`c.expr` without a free-name check,** for now.
- **Errors are SCXML's.** `error.execution` for errors in the chart,
  `error.communication` for a failed send or invoke, through the core's
  inbound error path.

## 12. Open questions

1. **Text expressions in a native system.** Charts written as XML (or
   with string expressions) carry ECMAScript text. A native system either
   refuses them (text charts go to a quickjs system), or compiles each
   text once into a function, which is what `ecmascript.mjs` did.
2. **An invoke after its error.** Does the external invoke stay
   registered after `error.communication` until the state exits, or should
   the error end it?
3. **A faulted invoked chart.** The invoker gets no event today
   (`schedule.c:205`). Whether it should get `error.communication` is a
   core decision.
4. **`TA_IO_PROCESSOR_SYNC`.** Whether JS I/O processors need it; a JS
   processor would have to answer synchronously.
5. **Error data.** SCXML lets an error event carry data (SCXML.md:853);
   the core drops what `TA_EVENT_IN_ERROR_COMMUNICATION` brings
   (`schedule.c:370`). Granary's 0.1 errors carried `{ sendID, type,
   error }`.
6. **The link protocol in C,** later, if a second binding needs links.

## 13. Order of work

Each step is one or more commits that pass `mise run test`.

1. Bind `address.h` and `frame.h`; delete the JS twins.
2. `ta_host_pump`; `binding.c`'s `run` calls it.
3. Containers and adoption by content hash in C.
4. Trace policy by name in C.
5. The core changes of section 8, each with C tests.
6. I/O processors and invokers on today's binding, so granary can move
   first.
7. The chart object, the helpers and `c.expr`, with their unit tests.
8. The benchmark scenario, measured on today's binding.
9. The native ECMAScript implementation (from the "xstate" vtable) and
   quickjs in the binding; `ecmascript.mjs` removed.
10. The switchover, in commits: the native `System` and its JS class; the
    actor API; tests and examples migrated; the dropped API deleted;
    `surface.json` and both `index.d.ts` updated. `mise run test:full`.
11. Measure again; record both in the commit message.
