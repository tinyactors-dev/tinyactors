# A thin JavaScript binding over a deep native one

**Status:** Sketch (2026-10-10).
**Touches:** tinyactors-core: `bindings/shared/core.mjs` (rewritten),
`bindings/shared/binding.c` (narrowed and deepened),
`bindings/shared/ecmascript.mjs`, `bindings/node`, `bindings/web`,
`bindings/surface.json`, `examples/`; `src/host` (run loop policy,
containers), `src/trace` (rule parsing by name).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Summary

`bindings/shared/core.mjs` is 3555 lines. About a fifth of it emulates
xstate on top of SCXML, an eighth duplicates C that already exists, and
another eighth is logic free of any JavaScript API that belongs in C. Its
hot path is expensive: one `actor.send` costs three native calls, about
twenty Node-API calls per guard or action, a call per link, and a rebuilt
snapshot plus a `JSON.stringify` for *every* observed actor, stepped or
not. On the web build each Node-API call is a wasm→JS crossing.

The proposal:

1. **The core and SCXML semantics are the source of truth.** xstate
   lends vocabulary only where its meaning is the same as SCXML's or the
   core's; where the meanings differ, we use SCXML's name and SCXML's
   meaning, and never emulate xstate. `setup({ actions, guards, actors,
   delays })` is the table of named implementations; the machine config is
   plain data that compiles element by element to SCXML and refers to
   implementations only by name. No inline functions in a chart, so the
   SCXML is fully readable and is the serialization format (sections 3
   and 4). The examples show what our system can do; they do not decide
   the API.
2. **Put the deep layer in C, inside the binding.** `binding.c` gets a
   `System` that owns the host block, the datamodel, the pump, the timer,
   and notification of actors that stepped. JavaScript does not drive the
   run loop.
3. **Move what is free of JavaScript further down,** into `src/host` and
   `src/trace`, and bind `address.h` and `frame.h` instead of keeping JS
   twins.
4. **Keep JavaScript for what Node-API is bad at:** promises and streams
   (stores, transports, the OTLP exporter), plus the ECMAScript evaluator.

Bindings are written by hand. Generating them from `surface.json` was
considered and rejected: what a thin JS layer still calls directly is
about fifteen small pure functions, less code than a generator and its
parameter annotations (section 9).

## 1. Whom it serves, who pays

- **Application developer, first.** Asks: why is a send slow with many
  subscribers? What does my chart do? Why did this guard fail? Gets: a
  send whose cost does not grow with the number of observed actors; one
  chart format, SCXML, written either as XML or as an xstate-style config,
  readable in full in `toSCXML()`, in traces and in errors; implementations
  that are ordinary named functions, with closures and imports. Loses:
  inline functions in the config, `after`, `reenter`, nested
  `snapshot.value`, tags, `onError` (section 4).
- **Binding author.** Asks: what must I write myself? Today: read 3555
  lines of JavaScript and port what is not about JavaScript. After: the
  run loop policy, containers and trace names are C; a binding writes the
  datamodel's value handling, the codec, timers and I/O, and its own
  version of `setup` (a name → function table).
- **Maintainers.** One implementation each of addresses, frames, the run
  loop and trace presets. One datamodel vtable instead of two. A JS layer
  readable in one sitting.
- **Capacity planner.** The JS binding's per-event cost becomes a measured
  number (section 8).

**Who pays.**

- *C embedder:* nothing per event or per actor. New C in `src/host` is
  policy the binding used to own; an embedder that drives `ta_system`
  directly does not link it.
- *Maintainers:* a switchover commit that replaces `core.mjs` and migrates
  every test and both examples, and a few hundred lines of C with tests.
- *Application code:* the examples move 58 inline `assign`s into `setup`
  and replace `after` and `reenter`. Granary uses the 0.1 API and is not
  affected.

**Out of scope.** Replacing `ecmascript.mjs` with quickjs. The link
protocol stays in JavaScript for now (section 6). Dynamic children
(`spawnChild`), which would need a core feature.

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

Nothing outside this repository depends on the current API. Its shape is
held in place by `bindings/node/test`, the conformance and W3C harnesses,
two benchmarks and two examples (chess, ui-elements).

## 3. What maps directly, and is kept

Each row compiles to the SCXML on its right with no generated names and no
emulated semantics. A row is here because the meaning on both sides is the
same, not because xstate users expect it.

### Charts

| xstate | SCXML and core |
|---|---|
| `setup({ actions, guards, actors, delays })` | the implementations table; `actors` maps `invoke` `src` names to charts |
| `createMachine({ id, initial, states })` | `<scxml name initial>`, nested `<state>` |
| `type: 'parallel' \| 'final' \| 'history'`, `history: 'deep'` | `<parallel>`, `<final>`, `<history type>` |
| `context: { … }` (a literal) | `<datamodel>` with `<data id expr="JSON">` |
| `on: { open: { target, guard, actions } }`, arrays of them | `<transition event cond target>` in document order |
| a transition with actions and no target | a targetless `<transition>` |
| event wildcards `'*'`, `'door.*'` | SCXML event descriptors |
| `always` | eventless `<transition>` |
| `entry`, `exit` | `<onentry>`, `<onexit>` |
| `guard: 'canOpen'`, `{ type, params }` | `cond="canOpen"`, params as JSON |
| `actions: 'track'`, `{ type, params }` | `<script>track({"tag":"door"})</script>` |
| `invoke: { id, src: 'name', input, onDone }` | `<invoke id src>` with `<param>`; `onDone` is a transition on `done.invoke.<id>` |
| `setup({ actors: { fetchUser: fromPromise(fn) } })`, invoked by `src: 'fetchUser'` | an external invoke (`<invoke type src>`): the core emits `TA_EFFECT_INVOKE` (`src/system/route.c:405`); a resolved promise is reported with `TA_EVENT_IN_INVOKE_DONE` (`done.invoke.<id>`); leaving the state emits `TA_EFFECT_UNINVOKE`, which aborts the promise's `signal`. A rejection is `error.execution` in the invoker, with `_event.invokeid` set (`src/system/schedule.c:392`). |
| `output` on a final state | `<donedata>` |
| `types` | TypeScript only |

### Actions and guards

| xstate | SCXML and core |
|---|---|
| `assign(…)`, used in `setup` | an action whose result the datamodel merges into the context |
| `raise(event)` | `<raise>` |
| `raise(event, { delay, id })` | `<send delay id>` to self |
| `sendTo(target, event, { delay, id })` | `<send target event delay id>` |
| `sendParent(event)` | `<send target="#_parent">` |
| `cancel(id)` | `<cancel sendid>` |
| `log(expr)` | `<log>` |
| `stateIn('#id')` | `In('id')` |
| `and`, `or`, `not` over named guards | one boolean `cond` expression |

### Actors and systems

| xstate | SCXML and core |
|---|---|
| `createActor(logic, { input, systemID })` | spawn with params; `systemID` (spelled by our acronym rule, not xstate's) is a named actor (`@name`) |
| `start`, `stop`, `send` | `ta_host_spawn`, `ta_host_terminate`, `ta_host_send` |
| `subscribe` | observers, called for actors that stepped |
| `actor.sessionID`, `system.get(id)` | `_sessionid`, `ta_host_actor_by_name` |
| `snapshot.context`, `.status`, `.output` | datamodel variables, actor info, done data |
| `toPromise`, `waitFor` | a few lines over `subscribe` |

The snapshot's state is the SCXML configuration, under SCXML's names:
`snapshot.configuration` (the active state ids, from `ta_actor_config`)
and `snapshot.in(id)` (the `In()` predicate). xstate's `value` and
`matches` mean something else (section 4).

### Kept, with a rule stated rather than emulated

- **Targets.** A state's key is its SCXML id and is unique in the chart,
  so `target: 'opened'` and `'#opened'` mean the same. Relative targets
  (`.child`, sibling lookup) go, and with them the target resolution code.
- **`input`.** SCXML's rule: invoke and spawn params overwrite the `<data>`
  of the same id, so `input: { x }` sets `context.x`. A function
  `context: ({ input }) => …` is not supported.
- **Events.** `send({ type, ...data })` splits into a name and
  `_event.data`. Implementations get `{ context, event, params }`, with
  `event` as `{ type, ...data }` built once per step in `bind_event`, not
  per evaluation. Event names are the core's (`done.invoke.<id>`,
  `error.execution`, `error.communication`); nothing is renamed, and the
  binding adds no event names of its own. Any failure inside an actor,
  including a rejected promise, is `error.execution`.
- **Persistence.** `getPersistedSnapshot()` and
  `createActor(logic, { snapshot })` mean export and adopt. The snapshot is
  our blob, not JSON.

## 4. What does not map, and goes

| xstate | Why |
|---|---|
| inline functions in a config | they forced the source tokenizer. The compiler rejects them: "name it in `setup`". |
| `reenter` | xstate's default differs from SCXML's external and internal transitions. `type: 'internal'` can be exposed as is if needed. |
| `after` | no SCXML element; it lowers to generated event names. `raise(e, { delay, id })` in `entry` and `cancel(id)` in `exit` say the same explicitly. |
| `snapshot.value`, `snapshot.matches(value)` | a nested object keyed by xstate's state paths, not SCXML's configuration. The snapshot gives the configuration and `in(id)` instead (section 3). |
| `tags`, `hasTag`, `meta`, `description` | no runtime meaning in SCXML |
| `can(event)` | needs the core to evaluate transitions without taking them |
| `onError` on `invoke` | SCXML has no per-invoke error transition. Errors are `error.execution`; a chart that cares which invoke failed tests `_event.invokeid` in its `cond`. (When an invoked *chart* faults, the core reports `TA_EFFECT_ERROR` to the host and reclaims the child, `src/system/schedule.c:205`; whether the invoker should also get `error.execution` is a core question, not a binding one.) |
| `spawnChild`, `stopChild`, `enqueueActions`, `emit` | no SCXML equivalent |
| `inspect` | traces (OTLP) answer it |
| `fromCallback`, `fromObservable`, `fromTransition` | not charts, and not a single result an external invoke reports |
| `provide`, the implicit default system, the global machine registry, `linkMachine` | emulation or global state |

An actor with an external invoke in flight cannot be hibernated or
exported: the promise lives in JavaScript. The sweep must skip it (to be
checked against `ta_host_sweep`).

### The example, after

```js
import { setup, assign, raise, cancel } from '@tinyactors/node';

const door = setup({
  guards:  { canOpen: ({ context }) => !context.locked },
  actions: {
    countOpen: assign(({ context }) => ({ opens: context.opens + 1 })),
    track: ({ params }) => analytics.track(params.tag),
  },
}).createMachine({
  id: 'door',
  initial: 'closed',
  context: { opens: 0, locked: false },
  states: {
    closed: { on: { open: { target: 'opened', guard: 'canOpen',
                            actions: ['countOpen', { type: 'track', params: { tag: 'door' } }] } } },
    opened: {
      entry: raise({ type: 'close' }, { delay: 5000, id: 'autoClose' }),
      exit: cancel('autoClose'),
      on: { close: 'closed' },
    },
  },
});
```

```xml
<scxml name="door" initial="closed" datamodel="ecmascript">
  <datamodel><data id="opens" expr="0"/><data id="locked" expr="false"/></datamodel>
  <state id="closed">
    <transition event="open" target="opened" cond="canOpen">
      <script>countOpen</script>
      <script>track({"tag":"door"})</script>
    </transition>
  </state>
  <state id="opened">
    <onentry><send event="close" delay="5000ms" id="autoClose"/></onentry>
    <onexit><cancel sendid="autoClose"/></onexit>
    <transition event="close" target="closed"/>
  </state>
</scxml>
```

Hand-written SCXML gets the same implementations:
`setup({ … }).fromSCXML(xml)`. A builder chart and an XML chart are the
same thing at run time.

## 5. The datamodel

One vtable, not two. Every chart uses the ECMAScript datamodel
(`ecmascript.mjs`). When a chart is attached, each expression is resolved
once:

- an identifier, or a call with a JSON argument, that names an entry of
  the implementations table becomes an indexed call to it;
- anything else is ECMAScript, compiled once by `ecmascript.mjs`;
- a name that is neither defined nor valid ECMAScript is an error at
  attach time, pointing at the chart's line.

An action's return value, when it is an object, is merged into the
datamodel variables. That is how `assign` works, and it serves SCXML
written by hand as well.

An evaluation is one Node-API call: the `event` object is built once per
`bind_event` and the `meta` (self, system) once per instance.

## 6. What moves into C

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
uses it today. It stays in JS in this proposal, rewritten over the C
address and frame code, and without `linkMachine`.

## 7. The native seam

`binding.c` stays procedural: a `System` is a native handle, and a JS
class of about 40 lines forwards its methods to `native.*(handle, …)`.
That class is what `index.d.ts` and the `surface` check see. A class
would not make the seam deeper, and it would grow the web build's
Node-API subset (`napi_define_class`, wrapping) for no gain. Besides the
`System`, the seam has a few pure functions.

**`System`** owns the host block and grows it on `EXHAUSTED`; holds the
datamodel and the codec; runs the pump in budgeted slices on a platform
`defer` and one platform timer for `next_deadline`, both passed in at
creation; and after each slice calls the observers of actors that
stepped, once each. Methods: `load` (image), `spawn`, `send`, `sendMany`,
`terminate`, `snapshot(session)` (the configuration as a typed array of
state indices, context, status, output), `invokeDone`/`invokeEvent` (to
report on external invokes), `hibernate`/`wake`, `export`/`adopt`,
`reload`/`migrate`, `flush`, `stats`, `metrics`, `configureTraces`,
`stop`, and the peer calls the link code needs.

**Pure functions,** about fifteen: compile, image info, addresses,
traceparent, span IDs, decoding traces and metrics, frames.

Trace and log bytes stay one `Uint8Array` per slice (ADR 0006). Events
handed to JavaScript (logs, done, errors, requests, external invokes and
uninvokes, peer envelopes) are
delivered by the `System` to callbacks given at creation, built only when
some exist.

## 8. The JS layer

What remains in `core.mjs`:

- `setup`, `createMachine` (config → SCXML through `writeXML`), the action
  creators: about 250 lines;
- `createActor`, `Actor` and snapshot objects over the native `System`,
  `toPromise`, `waitFor`: about 150;
- stores and hibernation I/O, the tagged value codec: about 200;
- links and the store link over transports: about 300;
- the OTLP exporter and telemetry options: about 150.

About 1000 lines, plus `ecmascript.mjs`.

### Measuring it

Before the switchover, add a JS scenario reporting throughput and memory
together (in `tools/bench_js.mjs` or the torture chamber): sends per
second and bytes per actor with 0, 10 and 1000 observed actors, on Node and
the web build. Measure today's binding, then the new one, side by side on
one machine.

## 9. Generated bindings: considered, rejected

`include/tinyactors.api.json` has every function's parameters and every
struct's layout per target, so a generator is possible. It would need
annotations the types do not carry (output arrays, grow-and-retry
buffers, owned blocks), and it would generate only the shallow part of the
API. A 1:1 binding of `system.h` and `host.h` would put memory management
and the run loop in JavaScript, which is how `core.mjs` grew. With the
deep `System` in C, the JS layer calls about fifteen pure functions
directly; writing them by hand is less code than the generator.
`surface.json` keeps its job as the checklist of what the bindings reach.

## 10. Decided, and still open

Decided:

- **Keep `bindings/shared`.** One `binding.c` compiled as the Node-API
  addon and as the wasm module, one `core.mjs` over both. Two bindings,
  one per target, would duplicate the datamodel, the payload tables and
  the pump, which is what this proposal removes.
- **No wasm everywhere.** Node keeps the native addon.
- **A procedural seam behind a small JS class** (section 7).
- **Errors are `error.execution`.** The binding invents no error events;
  `onError` is not supported (section 4).
- **No `snapshot.value`:** the snapshot is the configuration (section 3).

Open:

1. **Hibernating with an external invoke in flight.** Check that
   `ta_host_sweep` and `ta_host_export_actor` refuse such an actor, or
   add that to the core.
2. **A faulted invoked chart.** Today the invoker gets no event. Whether
   it should get `error.execution` is a core decision, independent of
   this proposal.
3. **The link protocol in C,** later, if a second binding needs links.

## 11. Order of work

Each step is a commit that passes `mise run test`.

1. Bind `address.h` and `frame.h`; delete the JS twins.
2. `ta_host_pump`; `binding.c`'s `run` calls it.
3. Containers and adoption by content hash in C.
4. Trace policy by name in C.
5. The benchmark scenario, measured on today's binding.
6. The switchover: the deep `System` and one datamodel in `binding.c`, the
   new `core.mjs`, tests and examples migrated, the dropped API deleted,
   `surface.json` and both `index.d.ts` updated. `mise run test:full`.
7. Measure again; record both in the commit message.

Steps 1–4 shrink today's `core.mjs` before it is replaced, so the
switchover is binding work only.
