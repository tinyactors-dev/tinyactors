# ADR 0039: Data a chart sends is copied by a copier in JavaScript

**Status:** Accepted and implemented (2026-09-30); the owner accepted the cost below, to be optimized later. Applies core ADR 0097; refines ADR 0011 (a value leaving a session is copied, not shared).
**Date:** 2026-09-30

## Context

SCXML C.1 says the data of an event one session sends another MUST be a
copy. napi's JavaScript data model shares every value by reference (ADR
0011): a chart that sends `this.cart` and then changes it changes what the
receiver holds, even while the event waits in its queue (gap G4 of the
distributed-systems proposal). Core ADR 0097 adds `copy_out`, a data-model
operation core calls on each value that leaves a session: `<send>` data to
anything but `#_internal`, an `<invoke>`'s params and content, autoforwarded
data. This ADR says what napi's `copy_out` answers.

A JavaScript value has no one copy. Three existing answers:

- **`structuredClone`**: copies plain data, `Date`, `Map`, `Set`, typed
  arrays and `Error`, keeps shared references and cycles, and throws on a
  function; a class instance comes back as a plain object, its prototype
  lost. It is also slow for small values: 490 ns for `{ by: 1 }` on Node
  against 18 ns for a hand-written copy (Bun: 68 against 23).
- **`v8.serialize`**, which offload uses (core ADR 0074): the same rules,
  and bytes in between.
- **napi's own vocabulary of values** (ADR 0031): plain data, the standard
  classes every system registers, and values that describe themselves
  with `[serialize]()` and are rebuilt from their registered class. The
  durable printer (ADR 0034) refuses the rest; offload keeps the rest by
  handle.

## Options

- **A. `structuredClone`, refusing what it refuses.** The platform's
  answer, but a class instance silently becomes a plain object, a chart
  that sends a callback (`{ reply: resolve }`) starts failing with
  `error.execution`, and it costs about half a microsecond per send.
- **B. A copier in `lib/` over ADR 0031's vocabulary, sharing what it
  cannot copy.** It copies what napi already knows how to rebuild, keeps
  prototypes, and shares the rest, as offload keeps the rest by handle.
- **C. Freeze instead of copying.** No copy at all, but the sender's own
  data freezes with what it sent, so its next assignment throws.

## Decision

**Option B.** `lib/values.js` builds one copier per system (`copier(classes)`,
over the system's registered classes), which `createSystem()` installs with
`setCopyOut`; `copy_out` in `src/napi_datamodel.c` calls it.

1. **Copied:** plain objects (prototype `Object.prototype` or `null`), their
   own enumerable string-keyed properties, read as values (a getter runs);
   arrays, holes kept; `Date`, `RegExp`, `Map`, `Set`, `ArrayBuffer`,
   `DataView`, the standard typed arrays and `Buffer`, a `Map`'s keys and
   values and a `Set`'s items included; and a value that describes itself
   (`[serialize]()`, ADR 0031), rebuilt with the class registered under its
   name from a copy of its arguments.
2. **Shared, as before:** primitives (they cannot change; an inline
   primitive does not even reach JavaScript), strings, bigints, symbols,
   functions, and objects nothing says how to copy: a class instance
   without `[serialize]()`, or whose name no class is registered under, a
   `Promise`, a `WeakMap`, a proxy's target. So no chart that worked before
   fails: what napi cannot copy it passes as it did.
3. **Shared references and cycles are kept:** an object met twice is copied
   once. The walk keeps its work in an array, not on the JavaScript stack;
   a plain object of primitives, the common case, is copied without it.
4. **A copy that throws** (a getter, a `[serialize]()`) is `error.execution`
   in the sender, with the thrown value as its data, and nothing is sent
   (core ADR 0097 point 4).
5. **Mail from JavaScript is not copied.** `post()`, `send()`, an I/O
   processor's `context.post()` and an invoker's events put a value into a
   session; the application hands it over, as a function argument, and
   core does not call `copy_out` for it (core ADR 0097 point 3). Neither is
   `raise()` (internal), done data (the child has ended), nor what
   `inspect()` and `data()` return (live data, documented as shared).

## Cost

Measured with `mise run bench-compare d795ce9` (the commit before this
change) on machine `a99e44a37e2e` (Node v22.23.2) and `c989bd79a51f` (Bun
1.4.1), which runs the tracing, input-log and a new send-data bench
(`test/bench/send_data.js`: 10,000 named actors in a ring, each `go` sends
one `hit` to the next actor, with no data, the number 1, a new `{ by: 1 }`,
or a new `{ by: 1, meta: { tags: ['a', 'b'], at: 5 } }`). Medians of
alternating runs, ns per event (one post, two macrosteps and one send):

| send data, 10,000 actors | Node before | Node after | change | Bun before | Bun after | change |
|---|---|---|---|---|---|---|
| no data | 936 | 930 | -0.6% | 853 | 856 | +0.3% |
| number | 1,010 | 1,005 | -0.5% | 912 | 925 | +1.4% |
| `{ by: 1 }` | 1,041 | 1,188 | **+14.2%** | 1,019 | 1,132 | **+11.2%** |
| nested, 3 containers | 1,048 | 1,466 | **+39.9%** | 1,020 | 1,383 | **+35.6%** |
| tracing bench, untraced, 100,000 actors | 461 | 465 | +0.7% | 397 | 397 | +0.2% |
| input-log bench, unlogged, 1,000 actors | 511 | 516 | +1.0% | 462 | 462 | +0.1% |
| input-log bench, logged | 3,770 | 3,769 | -0.0% | 2,966 | 2,964 | -0.1% |

(3 alternating runs each.) A first build, whose copier walked every value
and whose `copy_out` opened a full callback frame, measured +22.4% and
+21.3% for `{ by: 1 }`, and +34.4% and +37.0% for the nested value; the
fast path for a plain object of primitives and the lighter frame brought
the first down to the figures above.

Memory does not move: in the send-data bench, core bytes per actor (359),
native bytes per actor (442), heap bytes per actor (253 on Node, 166 on
Bun) and allocator calls per event (2.01) are the same on both sides, and
RSS per actor within 1.5% (1,627 to 1,648 on Node). A copy's box comes from
the box free list and is freed with the event.

Where the time goes, on Node, for `{ by: 1 }`: the copier itself is about
18 ns; calling it through Node-API (a handle scope, `typeof`, the call,
`strict_equals`) about 60 ns; a second box for the copy (a `napi_ref` made,
and the original's deleted) and the extra garbage the rest. A nested value
pays the walk: 180 ns for three containers.

**This is more than the 10% the plan allowed for the untraced path, for a
send whose data is an object.** A send without data, or with a primitive or
a string, costs what it did. The decision to accept it, or to take one of
the follow-ups below, is the owner's.

Possible follow-ups, none built:
- a system option that turns copying off (`createSystem({ copyEventData:
  false })`) for applications that never mutate what they send;
- skipping objects that are deeply frozen (a check per property, so only
  cheaper for large values);
- a copy that does not keep shared references (no memo map), which halves
  the nested case but duplicates a shared object.

## Consequences

- C.1 holds for charts on the built-in and on registered data models
  (ADR 0019): a mutation after a send, in the sender or the receiver, no
  longer shows on the other side. The W3C IRP suite passes 176 of 176 on
  Node and Bun.
- A receiver gets a new object per send. `event.data !== sent` where the
  two used to be one object.
- What cannot be copied is still shared, so C.1 does not hold for it; a
  class that should cross by value implements `[serialize]()` and is
  registered, as it must be to be printed or made durable anyway.

## Implementation notes

- **Where:** `lib/values.js` (`copier`), `lib/system.js` (installs it with
  `setCopyOut`), `src/napi_datamodel.{c,h}` (`tna_dm_copy_out`, the copier's
  reference, released at close), `src/values.{c,h}` (`tna_value_boxed`),
  `src/napi_loads.{c,h}` and `src/napi_system.c` (`setCopyOut`).
- **A lighter frame than other callbacks:** `copy_out` opens a handle scope
  only. The copier runs no chart code, so the call in progress (the
  evaluation context's host and instance) is left as it is; each Node-API
  call saved is on the path of every send with object data.
- **Tests:** `test/copy.js` on Node and Bun: between named actors (a
  `Date` and a `Map` copied, the sender's and the receiver's later
  mutations not visible to the other), a send to self copied when it runs
  and a `raise()` shared, what an I/O processor gets (a described value
  rebuilt, a function, a class instance and an unregistered described
  value shared, a `Buffer` copied), a copy that throws is `error.execution`
  and sends nothing, shared references and cycles kept, mail from
  JavaScript not copied, and an invoked child's params and `#_parent` data
  (on the example ECMAScript data model).

### The cost is accepted (2026-09-30)

The owner accepted the measured cost (+11 to 14% for `{ by: 1 }`, +36 to
40% for three nested containers; no change without data or for numbers;
memory unchanged) rather than an opt-out, skipping frozen values, or
dropping shared references. Core ADR 0097's note records that a data
model can make `copy_out` cheap, for instance by raising a reference count
on a value that cannot change. Making napi's copier cheaper is later
work: candidates are the options weighed above (frozen values shared,
fewer value holders per copy) and avoiding the second box.
