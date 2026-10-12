# Decisions

The design decisions the guide (`tinyactors-core/docs/bindings.md`) and
the headers leave open, each with its evidence: a scenario in this
design, and `path:line`, a primary source or a spike
(`.agents/skills/reasoned-analysis`). The commit that records one is the
commit that acts on it.

## Per-environment state is the environment's instance data

**Decision.** The addon's state for one environment (the main thread, or
a worker) is a `ta_napi_module` set with `napi_set_instance_data` at
module init and freed by its finalizer (`src/module.c`); nothing is a C
global (the lint rule `no-mutable-globals`).

**Scenario.** An application runs a host in the main thread and another
in a `Worker`, each loading the addon: a `napi_ref` (the
`TinyactorsError` class, a host's handlers) made in one environment
means nothing in the other, so a global holding one breaks the second
environment.

**Evidence.** A spike (2026-10-12, node v22.23.2 and bun 1.4.1, an addon
built with the pinned clang 22 and `-bundle -undefined dynamic_lookup`):
`napi_set_instance_data` returned `napi_ok` in both runtimes; a counter
in the instance data counted 1, 2 in the main thread, 1 in a worker that
loaded the addon, then 3 in the main thread: each environment has its
own. `napi_type_tag_object` and `napi_check_object_type_tag` told a
tagged object from a plain one in both. `test/load.test.mjs` keeps the
worker half of it.

## Errors: one class, TinyactorsError, with core's fields

**Decision.** What core refuses is thrown as `TinyactorsError`, an
`Error` the addon defines (`src/module.c`; in JavaScript, since a
Node-API class cannot extend `Error`), exported by the package:

- a refused request or config (`ta_host_request`'s reply, the
  `environment->error` of `ta_host_create`): its `message`, `code`,
  `reason` and `path`, unchanged (bindings.md, section 10: "An `error`
  becomes the language's exception, with its `code`, `reason`, `path` and
  `message` unchanged");
- a call that returned a status (`ta_host_spawn`, `ta_cbor_decode`, the
  value table refusing a value): `status`, the `ta_status` of `types.h`
  (-3 for `TA_ERROR_INVALID`), and as its message the status's name in
  `types.h` (`TA_ERROR_INVALID`), since core gives such a call no message
  and the binding writes none of its own (bindings.md, section 13: "No
  ... messages of the binding's own").

`decode` and `encode` refuse with it too: `status` -3
(`TA_ERROR_INVALID`) for bytes that are not one neutral value
(`ta_cbor_decode`'s refusal, or the value table's: another tag, another
simple value, a key that is not text) and for a value that is not one
(a cycle, nesting past 64, a BigInt past 64 bits, a lone surrogate);
-7 (`TA_ERROR_UNSUPPORTED`) for an indefinite length, as
`ta_cbor_decode` answers (`include/tinyactors/cbor.h`, "TA_ERROR_UNSUPPORTED:
indefinite lengths"). An exception the value itself throws (a getter, a
`toJSON`) propagates unchanged.

**Scenario.** An application decodes bytes that arrived on a socket and
matches the failure: with one class and core's status it writes
`error instanceof TinyactorsError && error.status === -3`, the same test
as for a refused spawn; a request's refusal is matched on `code` and
`reason` (requests.md), as the guide says.

**Not chosen.** JavaScript's own classes (`TypeError` as `JSON.stringify`
throws for a cycle, `SyntaxError` as `JSON.parse` for bad input): they
would carry no status, so a caller could match only on the message,
which core does not write. A class per kind of failure: core has one
status space and one error reply shape, so two fields of one class say
it.

**Evidence.** `test/values.test.mjs` ("anything else is refused when
read", "a cycle and nesting deeper than 64 are refused", "an exception
a getter throws while writing propagates").

## Strings are checked as UTF-16 in C

**Decision.** `values.c` reads a string as UTF-16
(`napi_get_value_string_utf16`), refuses a lone surrogate
(`TA_ERROR_INVALID`), and encodes UTF-8 itself, in one pass. It does not
call `String.prototype.isWellFormed` (which AGENTS.md named as the check
before this decision amended it): the same test in C costs no call into JavaScript per string, and
the UTF-8 that Node-API would write (`napi_get_value_string_utf8`)
replaces a lone surrogate with U+FFFD silently, so the addon could not
use it unchecked anyway (bindings.md, section 9).

**Evidence.** values.txt "invalid a lone surrogate in text", "... in a
key", and `test/values.test.mjs` ("text is a string; a lone surrogate is
refused when written"), under node and bun.

## The Host's API

**Decision.** Each item is a host input, a request or the handling of an
effect (`docs/api-map.md` has the core behind each). The shapes borrow
xstate's where it has one (`createActor(logic, {clock})`, `actor.send`,
`SimulatedClock`), never its meaning.

```js
import { Host, SimulatedClock, compile } from '@tinyactors/node';

const clock = new SimulatedClock();               // optional
const host = new Host({
  config: { systemID: 'app', system: { ticksPerSecond: 1000 } }, // host-config.md
  datamodel: 'native',                            // or 'quickjs'
  quickjs: { realm: 'auto' },                     // ta_datamodel_ecmascript_config
  clock,
  on: { log, done, error, peer, peerError },      // effect handlers
});
const segment = host.addImage(compile([{ name: 'counter.scxml', text }]));
host.boot(segment);
const session = host.spawn('counter', { name: 'a', params: { n: 1 } });
host.send(session, 'inc', { by: 2 });
host.sendMany([{ session, event: 'inc' }]);       // the accepted prefix's length
host.terminate(session);
host.request('actor.list', { limit: 2 });          // the result, or TinyactorsError
const peer = host.addPeer({ id: 'p', trusted: true });
host.deliver(peer, bytes);                         // a TA_ROUTE_* outcome
clock.increment(50);
host.close();
```

- **`config` is a value**, written as CBOR by the value table and passed
  to `ta_host_create`; a refusal throws `TinyactorsError` with the
  error `environment->error` holds (bindings.md, section 3).
- **`request(method, params)`** builds `{"method", "params", "id"}` from
  the caller's values and returns the `result` or throws the `error`
  (bindings.md, section 10). There is no item that takes a request's
  bytes: it would be a second way to call `ta_host_request`, made for
  the suite, as the Python binding argues too
  (`tinyactors-cpython/ROADMAP.md`, "Requests to core", 1). The vectors
  it cannot send are a request to core (ROADMAP.md).
- **One datamodel per host**, `datamodel: 'native'` (the addon's, the
  default: the guide's native datamodel comes first, bindings.md section
  8) or `'quickjs'` (`ta_datamodel_ecmascript`, configured by
  `quickjs`). Both vtables are named `"ecmascript"`, and core binds a
  chart to the first vtable whose name matches
  (`tinyactors-core/src/system/system.c:221-227`), so a second one would
  never be reached; `tinyactors-test/docs/driver.md` ("The chart
  language") prescribes the same for the adapter. The payloads the
  binding makes for `spawn` and `send` are for that one datamodel.
- **Ids.** The API gives ids as `BigInt`: sessions, segments, peers
  (AGENTS.md: "ids are always `BigInt`"). It takes a `BigInt`, or a
  number that is a safe integer, because a request's reply follows the
  value table, which gives an integer within ±(2^53-1) as a number
  (`docs/values.md`): `actor.list`'s `sessions` are numbers. The two
  meet only at the API's inputs; comparing a session from `spawn` with
  one from a reply needs `BigInt(n)`. (Open for the API review: the
  value table is one conversion, and a reply's ids are not marked as
  ids.)
- **Effects go to handlers** in `on`, called while the loop handles the
  effect, before the ack, with one record per effect read with the
  accessors (bindings.md, section 6), with the names of `system.h`'s
  field use and of the Python binding's handlers (`on_log(session,
  label, text)`, `on_done(session, data)`, `on_error(session, code)`):
  `log` `{session, label, text}` (`TA_EFFECT_LOG`: auxiliary, event),
  `done` `{session, name, data, final}` (`TA_EFFECT_DONE`: target, the
  payload through its datamodel's `to_wire` and the value table, and the
  auxiliary, the id of the `<final>` that ended it), `error` `{session,
  code}` (`TA_EFFECT_ERROR`: flags, the fault code), `peer` and
  `peerError` `{peer, envelope}` (`TA_HOST_EFFECT_PEER`,
  `TA_HOST_EFFECT_PEER_ERROR`: the bytes `ta_host_envelope` writes). A
  kind without a handler builds no record.
- **A handler that throws:** the loop acks, finishes the turn and throws
  the first error then (bindings.md, section 3). On the runtime's clock
  the turn runs in a `setImmediate` or `setTimeout` callback, so the
  error is the runtime's uncaught exception, as an exception in any
  callback is; with a `SimulatedClock` it is thrown by the input or
  `increment` that ran the turn, after core took the input.
- **The turn.** Without a clock, an input arms a turn on the event loop
  (`setImmediate`, `src/loop.c`), so the inputs of one tick share it; the
  loop runs every status of bindings.md section 3, and arms one
  `setTimeout` for the next deadline, ref'd while it is pending. With a
  `SimulatedClock`, no timer is armed: an input runs the turn at the
  clock's time before it returns, and `increment(ms)` moves the time and
  runs the turn until idle (bindings.md, section 3; ROADMAP.md, M1).
- **Reentrancy.** An input made while the loop runs (from a handler, or
  from a native expression during a run) is taken by core and sets a
  flag; the outer loop runs again when its run is idle, and no turn runs
  inside another (a nested `ta_host_run` would end the borrowed effects,
  `host.h:44-50`, or answer `TA_RUN_FAULT` during a run, `host.h:145`).
  `close()` from a handler marks the host, and the loop frees it when it
  returns (bindings.md, section 5).
- **`compile(charts, {resolve})`** is `ta_compile_charts` and
  `ta_compile_image`: `Uint8Array` image bytes, or `TinyactorsError` with
  core's `diagnostics` (`ta_compile_diagnostics`: severity, code, line,
  column, file, message). `resolve({base, source})` answers a static
  `src` (`ta_compile_options.resolve`) with `{name, text}`; undefined is
  `TA_ERROR_INVALID`, and an exception it throws propagates. The
  diagnostics of a compile that succeeds (warnings) are not given, and
  `TA_COMPILE_WERROR`, `TA_COMPILE_STRIP_DEBUG` and `ta_compile_system`
  (manifests) are not exposed in M1 (`docs/api-map.md`). `addImage(image)`
  opens a copy the host keeps until `TA_HOST_EFFECT_SEGMENT_FREED` or
  `close()`.

**Scenario.** The suite's basics scenario through this API (the adapter):
`compile` and `addImage` per `image` step, `spawn(chart, {name, params})`,
`send`, `sendMany` with its count checked, `clock.increment` for
`advance`, the `log` handler for `Outcome`; and an application's test
of its chart with `SimulatedClock`, where every effect has happened when
`send` returns (`test/host.test.mjs`).

**Not chosen.** A `Host` per segment or a factory function: one
`ta_host` is one object. An `EventEmitter` for effects: it is
JavaScript above the addon. `send(event)` with xstate's `{type, ...}`
object: core's event has a name and data, not fields to merge.

**Refutation.** A fresh agent refuted the first version of this section
(2026-10-12): it found the vectors no `request(method, params)` can send
(now a request to core), ids given in two types (now said above), the
record field `state` (now `final`, as the header names it), and the
reentrancy and handler-error cases left unsaid (now said). It found the
synchronous turn on a `SimulatedClock`, the one datamodel per host and
`compile`'s mapping consistent with the guide.

## The run budget

**Decision.** Each `ta_host_run` gets 100,000 fuel (`TA_NAPI_RUN_BUDGET`,
`src/loop.c`), the budget `ta-conform`'s scenario loop uses
(`tinyactors-core/tools/conform/scenario.c`, `settle`). A run that
yields gives the event loop a turn (`setImmediate`) before the next.

**Why it is the binding's.** The guide asks the binding to bound each
turn and yield to the runtime's event loop (bindings.md, section 3,
"Bound each turn"), and core has no default budget (`ta_host_run` takes
it from its caller, `host.h`): it is how long one macrotask of this
binding's loop may hold the event loop, I/O behaviour like the timer, not
a policy of core's. With a `SimulatedClock` the loop runs until idle
whatever the budget.
