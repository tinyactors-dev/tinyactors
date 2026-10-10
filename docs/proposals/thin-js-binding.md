# A thin JavaScript binding over a deep native one

**Status:** Sketch (2026-10-10).
**Touches:** tinyactors-core: `bindings/shared/core.mjs` (rewritten),
`bindings/shared/binding.c` (narrowed and deepened),
`bindings/shared/ecmascript.mjs`, `bindings/node`, `bindings/web`,
`bindings/surface.json`, `examples/`, `tests/w3c`; `src/system` (errors
for failed external invokes, snapshots with external invokes), `src/host`
(run loop policy, containers), `src/trace` (rule parsing by name).
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
   in the JS API is SCXML's or the core's, with the same meaning. Nothing
   is emulated, and the binding invents no events, fields or mechanisms.
2. **Charts are SCXML, in two syntaxes.** XML, and a chart object: the
   same elements and attributes as plain JS objects, because JavaScript
   developers do not read XML. The two are lossless in both directions;
   the W3C suite run through both proves it (section 3).
3. **The application reaches charts only through SCXML's extension
   points:** I/O processors for `<send type>` and invokers for
   `<invoke type>`, registered on the system (section 5). There are no
   application functions in a chart's scope: conditions and assignments
   are ECMAScript over the chart's own data, so the JS and C datamodels
   keep the same semantics.
4. **Put the deep layer in C, inside the binding.** `binding.c` gets a
   `System` that owns the host block, the datamodel, the pump, the timer,
   and notification of actors that stepped. JavaScript does not drive the
   run loop.
5. **Move what is free of JavaScript further down,** into `src/host` and
   `src/trace`, and bind `address.h` and `frame.h` instead of keeping JS
   twins.
6. **Keep JavaScript for what Node-API is bad at:** promises and streams
   (stores, transports, I/O processors, invokers, the OTLP exporter), plus
   the ECMAScript evaluator.

## 1. Whom it serves, who pays

- **Application developer, first.** Asks: what does my chart do? Why did
  this condition fail? How does my chart reach the outside world? Why is
  a send slow with many subscribers? Gets: charts as JS objects that mean
  exactly what the SCXML specification says, convertible to and from XML;
  the outside world through I/O processors and invokers, the mechanism
  SCXML defines for it; a send whose cost does not grow with the number
  of observed actors. Loses: xstate's config language, application
  functions inside charts, xstate's snapshot (section 6).
- **Granary, the reference application** (`docs/roles.md`), gets back the
  extension point it builds on: its I/O processors (the GitHub outbox,
  `src/lib/server/io/github.ts:42`; catch-up,
  `src/lib/server/github/catchup.ts:106`; the processors in
  `src/lib/ops/io/`) reply to the sender's address, which section 5
  provides as `origin`. It types an `Invoker` field
  (`src/lib/ops/feature.ts:65`) but registers none.
- **Binding author.** Asks: what must I write myself? After this: the
  datamodel's value handling, the codec, timers, and the I/O processor and
  invoker interfaces in the language's terms. The run loop policy,
  containers and trace names are C; the chart object's mapping is a fixed
  table any binding can copy.
- **Maintainers.** One implementation each of addresses, frames, the run
  loop and trace presets. One datamodel vtable instead of two, with the
  same semantics as the C datamodel. A JS layer readable in one sitting.
- **Capacity planner.** The JS binding's per-event cost becomes a measured
  number (section 9).

**Who pays.**

- *C embedder:* nothing per event or per actor. The core changes (section
  7) widen an existing inbound flag and refuse one more snapshot case.
- *Maintainers:* a switchover commit that replaces `core.mjs` and migrates
  every test and both examples, and a few hundred lines of C with tests.
- *Application code:* the examples are rewritten as chart objects plus I/O
  processors.

**Out of scope.** Replacing `ecmascript.mjs` with quickjs. The link
protocol stays in JavaScript (section 8). Dynamic children, and invoking a
chart of another segment (`src` resolves within the invoker's image),
which would both need core features.

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
session serial back out of the `sessionid` string. It reads
`_ioprocessors` (`binding.c:1406`) but offers no way to register an I/O
processor, and does not handle `TA_EFFECT_INVOKE` or `TA_EFFECT_UNINVOKE`.

## 3. Charts: SCXML as JS objects

The chart object is SCXML's document tree as JavaScript values, under a
mechanical mapping:

- **One object per element:** `tag` is the element's name, `children`
  its child elements. Neither is an SCXML attribute name, so they cannot
  clash with one; the element name cannot be a key itself, because
  `datamodel`, `initial` and `content` are both elements and attributes.
- **Attributes are the other keys,** under SCXML's names and with SCXML's
  values, every one of them, including the expression forms (`eventexpr`,
  `targetexpr`, `typeexpr`, `delayexpr`, `srcexpr`, `sendidexpr`,
  `idlocation`, `namelist`) and `binding`, `autoforward`, `type`. A value
  is the attribute's text: expressions are ECMAScript text, as in XML.
  Text content (a `<script>`, a `<content>`, a `<data>`) is `text`; XML
  content (elements of another namespace in a `<content>` or `<data>`,
  which the datamodel parses as a DOM) is `xml`, kept as markup.
- **Children are arrays in document order.** Where SCXML's semantics
  depend on order (states, transitions, executable content, several
  `<onentry>` blocks), the array keeps it.
- **Everything SCXML has:** `<scxml>`, `<state>`, `<parallel>`,
  `<final>`, `<history>` with its default `<transition>`, `<initial>`,
  `<transition>`, `<onentry>`, `<onexit>`, `<datamodel>` and `<data>`
  (with `expr`, `src` or content) on any state, `<invoke>` (several per
  state) with `<param>`, `<content>` and `<finalize>`, `<donedata>`, and
  the executable content `<raise>`, `<send>`, `<cancel>`, `<assign>`,
  `<log>`, `<script>`, `<if>`/`<elseif>`/`<else>` and `<foreach>`.

The compiler (object → SCXML through `ta_xml_writer_*`) and the reader
(SCXML → object, from the compiler's parse) follow these rules alone;
TypeScript types describe each element's attributes and children. The
door:

```js
import { chart } from '@tinyactors/node';

const door = chart({
  tag: 'scxml', name: 'door', initial: 'closed', datamodel: 'ecmascript',
  children: [
    { tag: 'datamodel', children: [
      { tag: 'data', id: 'opens', expr: '0' },
      { tag: 'data', id: 'locked', expr: 'false' },
    ] },
    { tag: 'state', id: 'closed', children: [
      { tag: 'transition', event: 'open', cond: '!locked', target: 'opened', children: [
        { tag: 'assign', location: 'opens', expr: 'opens + 1' },
        { tag: 'send', event: 'opened', type: 'https://granary.dev/io/audit', namelist: 'opens' },
      ] },
    ] },
    { tag: 'state', id: 'opened', children: [
      { tag: 'onentry', children: [{ tag: 'send', event: 'close', delay: '5s', id: 'autoClose' }] },
      { tag: 'onexit', children: [{ tag: 'cancel', sendid: 'autoClose' }] },
      { tag: 'transition', event: 'close', target: 'closed' },
    ] },
  ],
});
```

This is the same document as:

```xml
<scxml name="door" initial="closed" datamodel="ecmascript">
  <datamodel><data id="opens" expr="0"/><data id="locked" expr="false"/></datamodel>
  <state id="closed">
    <transition event="open" cond="!locked" target="opened">
      <assign location="opens" expr="opens + 1"/>
      <send event="opened" type="https://granary.dev/io/audit" namelist="opens"/>
    </transition>
  </state>
  <state id="opened">
    <onentry><send event="close" delay="5s" id="autoClose"/></onentry>
    <onexit><cancel sendid="autoClose"/></onexit>
    <transition event="close" target="closed"/>
  </state>
</scxml>
```

`chart(object)` and `fromSCXML(xml)` give the same thing; `.toSCXML()`
and `.toObject()` convert either way.

**Lossless, proven by the W3C suite.** `tests/w3c/run_js.mjs` runs every
W3C chart twice more: converted to an object and back to XML, and
compiled from the object directly. Both must pass exactly the tests the
XML passes. A unit test checks object → XML → object is the identity.

The chart object is a syntax, not a second language: it adds no element,
attribute, default or meaning, and the compiler reports errors at the
element's path in the object as it reports lines in XML.

## 4. Actors and systems

Core vocabulary, with SCXML's names for what SCXML defines.

| JS | Core and SCXML |
|---|---|
| `createSystem({ systemID, clock, ioProcessors, invokers, hibernation, restore, … })` | `ta_host_new` (`system_id`, `io_processors` in `ta_host_config`) |
| `system.spawn(chart, { name, params })` → `Actor` | `ta_host_spawn`: the actor starts at once. `params` overwrite the `<data>` of the same id, as invoke params do. Every actor has a name: the given one, else the chart's name (`system.c:531–536`). |
| `system.get(name)` | `ta_host_actor_by_name`: an actor with that name, which may be any actor of a chart of that name |
| `actor.send(event, data)` | `ta_host_send`; the session sees `_event` as SCXML defines it (`name`, `type`, `sendid`, `origin`, `origintype`, `invokeid`, `data`, and `raw` when given) |
| `actor.terminate()` | `ta_host_terminate`: the actor stops at the next microstep boundary (`core.c:1767–1772`), running its `<onexit>` handlers |
| `actor.configuration` | the active state ids (`ta_actor_config`) |
| `actor.in(id)` | `In(id)` |
| `actor.datamodel` | the session's variables |
| `actor.done` | a promise for a root actor: resolved with the donedata on its `TA_EFFECT_DONE`, rejected on a `TA_EFFECT_ERROR` for its session (a fault; the actor was reclaimed) |
| `actor.subscribe(observer)` | `observer(actor)` is called once after each slice in which the actor took a step; several steps of one slice are seen once |
| `actor.sessionID`, `actor.name`, `actor.address` | `_sessionid`; `_name`, which the core sets to the actor's name (SCXML says the document's `name`; the core differs, `system.c:474`); its `tinyactors://` address |
| `actor.export()` → blob | `ta_host_export_actor`: the actor **leaves** the system |
| `system.adopt(blob)` | `ta_host_adopt`: refused while the blob's session exists |
| `system.snapshot()`, `createSystem({ restore })` | `ta_host_snapshot` / `ta_host_restore`, the whole host |
| `system.hibernate()`, `hibernation: { store }` | `ta_host_sweep`, `ta_host_hibernate`, `ta_host_wake` |
| `system.reload(chart)`, `system.migrate(…)` | `ta_host_add_image`, `ta_host_migrate` |
| `system.flush()` | with a `clock` option: advance the logical clock and run until idle |

Event names are the core's: `done.invoke.<id>`, `done.state.<id>`,
`error.execution`, `error.communication`, `error.platform.upgrade`. The
binding adds none and renames none.

## 5. I/O processors and invokers

SCXML gives a host two extension points. The core implements both; the
binding reports back only through inbound flags the core defines.

```js
createSystem({
  systemID: 'granary',
  ioProcessors: {
    'https://granary.dev/io/github': {
      location: 'github',
      send({ target, event, sendid, data, origin }, system) { … },
    },
  },
  invokers: {
    'https://granary.dev/invoke/fetch': {
      start({ invokeid, src, params }, { send, done, fail, signal }) { … },
      // optional: receive(event) for sends to #_<invokeid>
    },
  },
});
```

**I/O processors** (`<send type="…">`). Registered at creation:
`ta_host_config.io_processors` is fixed for the host's life (it is given
again at restore). The host adds its own processor at index 0 and allows
16 in all (`host.c:283`). Each appears in `_ioprocessors` with its
`location`.

| Processor | Core |
|---|---|
| `send(message, system)`: `target`, `event`, `sendid`, `data`, and `origin`, the sender's address | a `TA_EFFECT_SEND` whose `io_processor` is this processor's index; a send with a `type` goes to its processor whatever the `target` (`core.c:1003–1007`) |
| it throws, or its promise rejects | `TA_EVENT_IN_ERROR_COMMUNICATION` with the `sendid`: `error.communication` in the sender's internal queue (SCXML §6.2). A late failure starts a macrostep of its own (`core.c:1847–1852`). |
| `system.get(…).send(…)`, or a send to `origin` | an event into the system; an address of another system goes through the router |

**Invokers** (`<invoke type="…">`). The `type` is a URI the application
chooses (SCXML §6.4); the core turns any `<invoke>` with a non-SCXML type
into an external invoke (`src/system/route.c:405`).

| Invoker | Core |
|---|---|
| `start({ invokeid, type, src, params }, ctx)` | `TA_EFFECT_INVOKE` (`event` = type, `target` = src, `sendid` = invokeid, `payload` = params); the binding keys it by segment and token, since tokens are per segment |
| `ctx.send(event, data)` | `TA_EVENT_IN_FROM_INVOKE`: the core sets `_event.invokeid` (`schedule.c:392`) |
| `receive(event)`: sends to `#_<invokeid>`, and autoforwarded events | `TA_EFFECT_SEND` carrying the invoke token (`route.c:118–121`, `621–626`) |
| `ctx.done(data)` | `TA_EVENT_IN_INVOKE_DONE`: `done.invoke.<id>` with `data` |
| `ctx.fail()`, or `start` throws or its promise rejects | `TA_EVENT_IN_ERROR_COMMUNICATION \| TA_EVENT_IN_FROM_INVOKE`: `error.communication` in the invoker's internal queue with `_event.invokeid` set (SCXML §3.12.2: errors arising from `<invoke>`). Needs the core change in section 7. |
| the invoking state exits | `TA_EFFECT_UNINVOKE`: `signal` aborts; the request is freed (`route.c:585–596`), so later `send`, `done` or `fail` calls are dropped (SCXML §6.4) |

An actor with an external invoke in flight is not hibernated, swept or
exported; the core refuses it (`hibernate.c:136–150`,
`system.h:404–405`). A whole-host snapshot is refused too (section 7).

## 6. What goes

| Today | Why |
|---|---|
| `createMachine`, `setup` and xstate's config (`context`, `input`, `on`, `always`, `after`, `entry`/`exit`, `onDone`, `onError`, `reenter`, relative targets, `type: 'parallel'`) | the chart object is SCXML's tree under SCXML's names (section 3) |
| inline functions, named guards and actions | the application reaches a chart through I/O processors and invokers (section 5); functions in a session's scope would be a feature only the JS datamodel has |
| `assign` (xstate), `raise`, `sendTo`, `sendParent`, `cancel`, `log`, `stateIn`, `and`/`or`/`not` builders | executable content and `cond` text in the chart object |
| `getSnapshot`, `.value`, `.matches`, `.status`, `.context`, `.output`, `.hasTag`, `.can` | `configuration`, `in()`, `datamodel`, `done` (section 4); xstate's status values do not exist in the core |
| `getPersistedSnapshot`, `createActor(logic, { snapshot })` | they read without removing; the core's export removes the actor. `export` and `adopt` say what happens. |
| `createActor` + `start`, `stop`, xstate's system ID option | `system.spawn`, `terminate`, `name` |
| events as `{ type, ...data }` | `_event.type` is SCXML's event type; an event is a name and data |
| `fromPromise`, `fromCallback`, `sendBack` | an invoker (section 5) |
| `tags`, `meta`, `description`, `inspect`, `toPromise`, `waitFor`, `provide` | no core or SCXML meaning; `actor.done`, `subscribe` and traces cover the uses |
| the implicit default system, the global machine registry, `linkMachine` | global state, or a chart used as plumbing |

## 7. Core changes

1. **A failed external invoke.** `TA_EVENT_IN_ERROR_COMMUNICATION`
   reports a failed `TA_EFFECT_SEND` and ignores the token
   (`schedule.c:358`). With `TA_EVENT_IN_FROM_INVOKE` as well, it reports
   a failed invoke: the token is checked first, as for other invoke
   events (`schedule.c:383–390`, and before the hibernated branch at
   `:342`), a stale token is refused, and the raised `error.communication`
   gets the invoke's `invokeid` (`ta_core_raise_platform` takes only a
   `sendid`, `core.c:2069`). The flag alone keeps its meaning: a failed
   send to `#_<invokeid>` carries the invoke token too
   (`system.h:211–212`).
2. **Snapshots with external invokes in flight.** `ta_host_snapshot`
   checks pending effects and migration jobs (`host.c:1733–1735`), not
   external invokes; a restored host would hold invokes no service
   answers. Refuse it with `TA_ERROR_STATE`, as hibernation and export do.

## 8. What moves into C

Each item deletes its JS twin in the same change.

1. **Addresses and frames.** Bind `address.h` and `frame.h` (`skip` in
   `surface.json` today).
2. **Run loop policy.** Retrying while `EFFECTS_FULL`, draining traces
   into the encoder before effects, `root_session`, job state: a new
   `ta_host_pump(host, now, budget, out)`, usable by every binding.
3. **Containers.** System snapshot and actor export records become a
   C-defined layout around an opaque datamodel section. `adopt` finds a
   segment by `ta_image_get_info.content_hash`, not by re-encoding JSON.
4. **Image lookups.** The current version per chart name comes from
   `ta_host_chart`; the segment and version maps in JS go. Invoke `src`
   resolves within the invoker's image.
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
holds the one datamodel vtable (over `ecmascript.mjs`, which keeps the C
datamodel's semantics) and the codec; runs the pump in budgeted slices on
a platform `defer` and one platform timer for `next_deadline`, both passed
in at creation; and after each slice calls the observers of actors that
stepped, once each. Effects for JS (sends to registered I/O processors,
invokes and uninvokes, logs, done, errors, peer envelopes) go to callbacks
given at creation, one call each. An evaluation is one call into
`ecmascript.mjs`; `_event` is built once per `bind_event`.

**Pure functions,** about fifteen, written by hand: compile, image info,
addresses, traceparent, span IDs, decoding traces and metrics, frames.

**What remains in `core.mjs`:**

- the chart object: the mapping table, object → SCXML, SCXML → object:
  about 250 lines;
- `createSystem`, `Actor` over the native `System`: about 150;
- I/O processors and invokers: about 100;
- stores and hibernation I/O, the tagged value codec: about 200;
- links and the store link over transports: about 300;
- the OTLP exporter and telemetry options: about 150.

About 1150 lines, plus `ecmascript.mjs`.

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
- **Hand-written bindings.** Generating them from `surface.json` would
  need parameter annotations the types do not carry and would generate
  only the shallow part of the API; a 1:1 binding of `system.h` and
  `host.h` would put memory management and the run loop back in
  JavaScript. `surface.json` stays the checklist of what the bindings
  reach.
- **Charts are SCXML,** as XML or as the lossless chart object.
- **The application reaches charts through I/O processors and invokers
  only.**
- **Errors are SCXML's.** `error.execution` for errors in the chart,
  `error.communication` for a failed send or invoke, through the core's
  existing effect-error path.

## 11. Open questions

1. **An invoke after its error.** Does the external invoke stay
   registered after `error.communication` until the state exits, or
   should the error end it? SCXML only says the error goes on the queue.
2. **A faulted invoked chart.** The invoker gets no event today
   (`schedule.c:205`). Whether it should get `error.communication` is a
   core decision.
3. **Actors that ended.** What the binding can still read after
   `TA_EFFECT_DONE`, a fault or `terminate`, and whether terminating a
   root actor produces an effect `actor.done` can settle on.
4. **`TA_IO_PROCESSOR_SYNC`.** Whether JS I/O processors need it: the run
   returns after such a send so the outcome lands in the sender's
   macrostep, which a JS processor would have to answer synchronously.
5. **Error data.** SCXML lets an error event carry data (SCXML.md:853);
   the core drops the data given with `TA_EVENT_IN_ERROR_COMMUNICATION`
   (`schedule.c:370`). Granary's 0.1 errors carried `{ sendID, type,
   error }`. Keep dropping it, or let the core pass it on.
6. **The link protocol in C,** later, if a second binding needs links.

## 12. Order of work

Each step is a commit that passes `mise run test`.

1. Bind `address.h` and `frame.h`; delete the JS twins.
2. `ta_host_pump`; `binding.c`'s `run` calls it.
3. Containers and adoption by content hash in C.
4. Trace policy by name in C.
5. The core changes of section 7, with C tests.
6. The chart object: the mapping table, both conversions, and the W3C
   round trip (section 3), on today's binding.
7. The benchmark scenario, measured on today's binding.
8. The switchover: the native `System`, one datamodel, I/O processors and
   invokers in `binding.c`; the new `core.mjs`; tests and examples
   migrated; the dropped API deleted; `surface.json` and both
   `index.d.ts` updated. `mise run test:full`.
9. Measure again; record both in the commit message.

Steps 1–7 shrink today's `core.mjs` or build beside it, so the switchover
is binding work only.
