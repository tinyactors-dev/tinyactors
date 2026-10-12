# @tinyactors/node

SCXML statecharts as actors, from Node.js and Bun: a Node-API addon that
drives the tinyactors host, with chart logic in JavaScript.

**Status: in development (milestone M1, [ROADMAP.md](ROADMAP.md)).** The
package is not published yet: it is built from this directory
(`mise run build`). The last published version, 0.2.0, was built from an
earlier binding with another API, which this package replaces.

## A host

```js
import { Host, SimulatedClock, compile } from '@tinyactors/node';

const counter = `<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript"
                        name="counter" initial="idle">
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="idle">
    <transition event="inc"><assign location="n" expr="n + _event.data.by"/><log label="n" expr="n"/></transition>
  </state>
</scxml>`;

const host = new Host({
  config: { systemID: 'app' },                 // tinyactors-core/docs/host-config.md
  on: { log: ({ session, label, text }) => console.log(session, label, text) },
});
host.addImage(compile([{ name: 'counter.scxml', text: counter }]));
const session = host.spawn('counter', { name: 'a' });
host.send(session, 'inc', { by: 2 });           // logs 1n n 2, on the next turn
```

- **`new Host({config, datamodel, quickjs, clock, on})`** creates one
  host from its config (`ta_host_create`). Charts whose `datamodel` is
  `"ecmascript"` run in this process's JavaScript engine (`datamodel:
  'native'`, the default), or in quickjs-ng inside the addon
  (`datamodel: 'quickjs'`, with `quickjs: { realm: 'auto' | 'isolated' }`).
- **`compile(charts, { resolve })`** compiles SCXML into an image's
  bytes; **`addImage(image)`** adds it as a segment (the host holds those
  bytes, not a copy, until the segment is freed: do not change them), **`boot(segment)`**
  spawns the actors a system image declares.
- **`spawn`, `send`, `sendMany`, `terminate`** are the host's inputs; ids
  are `BigInt`.
- **`request(method, params)`** is every rarely called method of
  `tinyactors-core/docs/requests.md` (`actor.list`, `actor.context`,
  `host.stats`, `trace.configure`, ...): its result, or a
  `TinyactorsError`.
- **`on`** takes the effects: `log({session, label, text})`,
  `done({session, name, data, final})`, `error({session, code})`, and
  `peer({peer, envelope})` and `peerError({peer, envelope})`, the
  envelope bytes for another process. `addPeer` and `deliver` take what
  comes back.
- **`close()`** frees the host; from a handler, after the turn.
- **`encode(value)`** and **`decode(bytes)`** are the package's CBOR, by
  the value table of [docs/values.md](docs/values.md): integers past
  2^53 as `BigInt`, bytes as `Uint8Array`, an actor reference as its
  address text.

What each item calls in core is in [docs/api-map.md](docs/api-map.md); the
design decisions, with their evidence, in
[docs/decisions.md](docs/decisions.md).

## Time

Without a clock, the host runs on the runtime's event loop: an input runs
on the next turn (`setImmediate`), and the host arms one `setTimeout` for
its next deadline (a delayed `<send>`, a timeout). **A pending deadline
keeps the process alive** under Node.js and Bun, and a host with none
lets it exit; a late timer only makes delayed sends late.

With a `SimulatedClock` the host arms no timer: an input runs the turn
before it returns, and `clock.increment(ms)` moves the time and runs the
host until it is idle, so a test sees every effect when the call
returns:

```js
const clock = new SimulatedClock();
const host = new Host({ config: { systemID: 'test' }, clock, on: { log } });
// ... spawn, send a <send delay="50ms"> ...
clock.increment(50);                            // the delayed send has fired
```

## Errors

What core refuses is a `TinyactorsError`: a refused request or config
carries core's `code`, `reason` and `path` (match on them, never on the
message); a refused call carries its `status`, which the package exports
by name (`ERROR_INVALID`, `ERROR_STATE`, ...). An exception a handler
throws ends the turn and is thrown after it: by the input or `increment`
under a `SimulatedClock`, as an uncaught exception on the event loop
otherwise.

## Limits

**Chart code is trusted.** The native datamodel runs a chart's
expressions in the application's realm (Node-API cannot make another),
with the application's globals in reach; it cannot be interrupted
mid-expression or limited in memory. Use `datamodel: 'quickjs'` for
charts you do not trust: quickjs runs them in its own engine, with an
operation limit.

**Values nest at most 64 deep** (core's `TA_CBOR_DEPTH_MAX`). That stops
a cycle, but not a value that shares subtrees (`a = [a, a]` sixty times
is 2^60 items when written), which is another reason chart code must be
trusted.

A `<script>` on the native datamodel runs as a strict function body:
its top-level declarations are local to it, unlike quickjs, which gives
such a chart sessions of their own.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
