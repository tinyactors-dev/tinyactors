# A thin JavaScript binding over a deep native one

**Status:** Sketch (2026-10-10).
**Touches:** tinyactors-core: `bindings/shared/core.mjs` (rewritten),
`bindings/shared/binding.c` (narrowed and deepened),
`bindings/shared/ecmascript.mjs` (removed), `bindings/node`,
`bindings/web`, `bindings/surface.json`, `examples/`, `tests/w3c`.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Summary

The task: take what `src/*` can do, as its public headers declare it, and
expose it through the Node-API binding and a thin JS API. The API's shape
borrows xstate's (`createActor`, `send`, `getSnapshot`, `subscribe`,
`system.get`); what it does is whatever the core does. No core changes.

Today `bindings/shared/core.mjs` (3555 lines) reimplements much of the
core in JavaScript, emulates xstate on top of SCXML, and leaves out what
the core offers for the outside world: I/O processors and external
invokes. One `actor.send` costs three native calls, about twenty Node-API
calls per guard or action, and a rebuilt snapshot for every observed
actor.

The new layering:

- **`binding.c`:** a native `System` over `ta_host` that owns the host
  block, the ECMAScript datamodel, the run loop and effect delivery; plus
  hand-written wrappers for the pure functions of `address.h`,
  `frame.h`, `trace.h` and `otlp.h`.
- **`core.mjs`:** the JS API below: chart helpers, `createSystem`,
  `createActor`, actors and snapshots, I/O processors and invokers,
  stores, links, the OTLP exporter. About 1300 lines.

## 1. Whom it serves

- **Application developer:** an API shaped like xstate's, over the
  core's semantics; charts in JS; the outside world through I/O
  processors and invokers; a send that costs one native call.
- **Granary** gets back I/O processors, which it uses through the 0.1 API
  (`src/lib/server/io/github.ts:42`, `src/lib/server/github/catchup.ts:106`).
- **Binding author:** the JS layer is a mapping of the headers, so a new
  binding copies the mapping, not 3500 lines of logic.
- **Maintainers:** one implementation of addresses, frames and the
  ECMAScript datamodel per engine, instead of a C one and a JS one.

## 2. Charts

Charts are SCXML. `fromSCXML(xml)` compiles XML as it is. In JS, helpers
named after the elements build the same document and write it through
`ta_xml_writer_*`:

```js
import { scxml as s } from '@tinyactors/node';

const c = s.chart<{ opens: number; locked: boolean }>();

export const door = c.scxml('door', { initial: 'closed', datamodel: 'ecmascript' },
  c.datamodel({ opens: c.expr(() => 0), locked: c.expr(() => false) }),
  c.state('closed',
    c.transition('open', 'opened', { cond: c.expr(({ locked }) => !locked) },
      c.assign('opens', c.expr(({ opens }) => opens + 1)))),
  c.state('opened',
    c.onentry(c.send('close', { delay: '5s', id: 'autoClose' })),
    c.onexit(c.cancel('autoClose')),
    c.transition('close', 'closed')),
);
```

- One helper per SCXML element. Identifying attributes are positional
  (`state` → `id`; `transition` → `event`, `target`; `assign` →
  `location`, `expr`; `send`, `raise` → `event`; `cancel` → `sendid`;
  `log` → `label`, `expr`; `if`, `elseif` → `cond`; `foreach` → `array`,
  `item`; `data`, `param` → `id`/`name`, `expr`; `script`, `content` →
  text), then an attributes object under SCXML's names, then children.
- `c.expr(fn)` marks code. `fn` takes one object: the session's variables
  and `_event`, `_sessionid`, `_name`, `_ioprocessors`, `In`. Expressions
  may also be strings.
- `system.load(charts, { documents })` compiles charts into one image
  (`ta_compile_charts`) and adds it (`ta_host_add_image`); `documents`
  answers `src` attributes. Loading a chart name again is a new version.

## 3. Datamodels

The core gives a chart the first datamodel of its system whose name
matches its `datamodel` attribute (`system.c:221–226`). A system chooses
the implementation of `"ecmascript"`:

| `createSystem({ ecmascript })` | Values | Expressions |
|---|---|---|
| `'native'` (default) | JS values as Node-API references | `c.expr` functions, called directly (closures work); string expressions compiled once into such functions when the chart is attached |
| `'quickjs'` | quickjs values (`src/datamodel_ecmascript`) | text; for untrusted charts and the W3C suite |

The native implementation is today's "xstate" vtable without its xstate
event renames and per-evaluation allocations. `ecmascript.mjs` is removed.

## 4. Systems and actors

| JS | Core |
|---|---|
| `createSystem({ systemID, ecmascript, clock, ioProcessors, invokers, logger, hibernation, restore, capacities })` | `ta_host_new` (`ta_host_config`: `system_id`, datamodels, `io_processors`, capacities); `ta_host_restore` with `restore` |
| `system.load(charts, { documents })` | `ta_compile_charts`, `ta_host_add_image`, `ta_host_chart` |
| `system.boot(manifest, { documents })` | `ta_compile_system`, `ta_host_add_image`, `ta_host_boot` |
| `createActor(chart, { system, input, systemID }).start()` | `ta_host_spawn(segment, chart, name = systemID, params = input)` |
| `system.get(systemID)` | `ta_host_actor_by_name` |
| `system.actors()` | `ta_host_actors` |
| `actor.send({ type, ...data })` | `ta_host_send`: the event's name is `type`, its data the object |
| `system.sendMany(events)` | `ta_host_send_many` |
| `system.send(address, event, options)` | `ta_host_send` with `ta_event_in` fields (`sendid`, `origin`, `origintype`, `raw`, `traceparent`); another system's address goes through the router |
| `actor.stop()` | `ta_host_terminate` |
| `actor.getSnapshot()` | `{ value, context, status, output }`: `value` from `ta_actor_config` and `ta_image_state_info` (nested, computed when read); `context` the session's variables; `status` from `ta_actor_info` (`TA_ACTOR_DONE` → `'done'`, a `TA_EFFECT_ERROR` → `'error'`, after `stop` → `'stopped'`, else `'active'`); `output` the donedata of `TA_EFFECT_DONE` |
| `actor.subscribe(observer)` | called after each run in which the actor may have changed: the binding knows which sessions it sent events to and which effects named them |
| `toPromise(actor)` | settles on `TA_EFFECT_DONE` or `TA_EFFECT_ERROR` |
| `actor.sessionID`, `actor.address` | `_sessionid`; `ta_address_format` |
| `system.flush()` | with a `clock`: `ta_host_advance_time` to `next_deadline`, `ta_host_run` until idle, repeated |
| `system.sweep(max)` | `ta_host_sweep`, then `ta_host_hibernate` per actor into `hibernation.store`; `TA_HOST_EFFECT_WAKE` loads and `ta_host_wake`s |
| `system.export(actor)` → blob, `system.adopt(blob)` | `ta_host_export_actor` (the actor leaves), `ta_host_adopt` |
| `system.snapshot()` | `ta_host_snapshot`, `ta_host_snapshot_segments` |
| `system.migrate(chartName, options)` | `ta_host_migrate`, `ta_host_job_pending`, `TA_HOST_EFFECT_MIGRATED`, `TA_HOST_EFFECT_JOB_DONE` |
| `system.stats()` | `ta_host_stats_get`, `ta_host_chart_stats` |
| `system.configureTraces(policy)`, `watchTraces`, `metrics()` | `ta_host_trace_set`, the `otlp` encoder (ADRs 0006, 0007) |
| `system.stop()` | `ta_host_free` |

Internal to the native `System`: `ta_host_run`, `ta_host_advance_time`,
`ta_host_effects`/`_ack` (the run loop, one platform timer for the next
deadline), `ta_host_resume` (an `<invoke src>` not in the image),
`ta_host_locate`, `ta_host_segment_*`, `ta_host_system`.

Effects go where the API says: `TA_EFFECT_SEND` with an I/O processor →
`ioProcessors`; `TA_EFFECT_INVOKE`/`UNINVOKE` → `invokers`;
`TA_EFFECT_LOG` → `logger`; `TA_EFFECT_DONE`/`ERROR` → snapshots,
`toPromise`; `TA_EFFECT_WAKE` → the store; peer effects → links.

## 5. I/O processors and invokers

```js
createSystem({
  ioProcessors: {
    'https://shop.dev/io/payments': {
      location: 'payments',
      async send({ target, event, sendid, data, origin }, { system, signal, traceparent }) { … },
    },
  },
  invokers: {
    'https://shop.dev/invoke/stock': {
      start({ invokeid, type, src, params }, { send, done, fail, signal }) { … },
      receive(invokeid, event) { … },
    },
  },
});
```

| JS | Core |
|---|---|
| an I/O processor | an entry of `ta_host_config.io_processors` (up to 16), shown in `_ioprocessors`; `<send type>` produces a `TA_EFFECT_SEND` with its index |
| a processor throws or rejects | `ta_event_in` with `TA_EVENT_IN_ERROR_COMMUNICATION` and the `sendid`: `error.communication` for the sender |
| an invoker | `<invoke type>` of a non-SCXML type produces `TA_EFFECT_INVOKE` (type, src, invokeid, params); its type is also registered as an I/O processor, since SCXML sends to an invoked service with the invoke's type and target `#_<invokeid>` (SCXML §6.4) |
| `send(event, data)` | `TA_EVENT_IN_FROM_INVOKE` (the core sets `_event.invokeid`) |
| `done(data)` | `TA_EVENT_IN_INVOKE_DONE`: `done.invoke.<id>` |
| `fail()`, or `start` throws or rejects | `TA_EVENT_IN_ERROR_COMMUNICATION` to the invoking session: `error.communication` |
| `receive(invokeid, event)` | sends to `#_<invokeid>` and autoforwarded events (`TA_EFFECT_SEND` with the invoke token, or for the invoke type's processor) |
| the state exits | `TA_EFFECT_UNINVOKE`: `signal` aborts |

## 6. Pure functions

Hand-written wrappers, exported as they are today but calling C instead
of JS twins:

| JS | Core |
|---|---|
| `parseAddress`, `formatAddress` | `ta_address_parse`, `_parse_local`, `_format`, `_format_name`, `_name_decode`, `_system_valid` |
| `encodeFrame`, `FrameDecoder` | `ta_frame_header`, `ta_frame_reader_init`, `ta_frame_read` |
| `parseTraceparent`, `formatTraceparent`, `stepSpanID` | `ta_traceparent_parse`, `_format`, `ta_step_key_span_id` |
| `decodeTraces`, `metricsToOTLP` | the `ta_otlp_next_*` readers, `ta_otlp_add_metric`, `_put_metrics` |
| `loadImage`, `image.info()` | `ta_image_open`, `ta_image_get_info`, `ta_image_chart_*` |

Links (`createLink`, `serveLinks`) keep their JS transports over
`ta_host_peer_*`, `ta_host_route`, `ta_host_export`, `ta_host_deliver`,
`ta_host_peer_error`, `ta_host_peer_status`, with frames and addresses
from C. The store link (`createStoreLink`) uses the core's store driver
(`ta_host_route_begin`/`_step`, `ta_host_message_init`,
`ta_host_envelope_json`, `ta_host_error_event`/`_route`) instead of its
JS reimplementation.

## 7. Not exposed

`system.h`'s single-system API (the host covers it), `core.h`,
`runtime.h`, `basic_http.h`, the datamodel vtable, the XML writer (used
by the helpers), and the OTLP encoder's internals.

## 8. What goes

The xstate config compiler and its function-source tokenizer,
`createMachine`/`setup`, `after`/`always`/`reenter` lowering, the
`xstate.*` event renames, `linkMachine`, the global machine registry and
default system, `ecmascript.mjs`, and every JS twin of C (addresses,
frames, wire format, trace presets, segment and version maps).

## 9. Order of work

Each step is one or more commits that pass `mise run test`.

1. Bind `address.h` and `frame.h`; delete the JS twins.
2. I/O processors and invokers on today's binding.
3. The chart helpers and `c.expr`, with unit tests.
4. A JS benchmark (throughput and memory together, native and quickjs,
   0/10/1000 subscribed actors), measured on today's binding.
5. The native ECMAScript implementation and quickjs in `binding.c`;
   `ecmascript.mjs` removed; the W3C suite under quickjs.
6. The native `System` and the API of section 4, in commits; tests and
   examples migrated; the dropped API deleted; `surface.json` and both
   `index.d.ts` updated. `mise run test:full`.
7. Measure again; record both in the commit message.
