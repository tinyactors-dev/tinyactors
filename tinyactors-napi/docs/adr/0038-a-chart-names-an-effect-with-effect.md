# ADR 0038: A chart names an effect with effect()

**Status:** Accepted and implemented (2026-09-30); see the implementation notes.
**Date:** 2026-09-30

## Context

A chart asks the outside world to do something with a `<send>` through an
I/O processor the application registered (`registerIO(type, processor)`,
core ADR 0068; README "Talking to the outside world"):

```js
t.send('charge', (send) => send.via('effects').data(({ event }) => event.data))
```

The chart names what it wants, `charge`, and the processor decides how:
it dispatches on `request.event`, calls a payment API, and posts the answer
back. Most such sends look the same: one processor for the application's
effects, the effect's name as the event, and its data. The ROADMAP's open
decision asks for a helper for naming effects that expands to
`send(...).via(...)`.

What the helper must keep:
- **It is a `<send>`.** Nothing new reaches core or the SCXML export: an
  exported chart shows `<send event="charge" type="effects">`, and the
  processor, error events and send IDs behave as for any send.
- **The builder's style.** Actions are methods of the action builder that
  append one action (`raise(event, data)`, `send(event, build)`), and a
  send's details are set in its block.
- **The typed event maps** (`statechart<Data, Events>()`): an effect named in
  the map has its data type-checked, as a send's data is.

## Options

- **A. An action method, `effect(name, data?, build?)`.** It appends
  `send(name, (send) => send.via('effects').data(data))`, then runs
  `build` on the same send builder, so `build` can change the processor
  (`via('payments')`), delay it, or give it an ID. One line per effect, the
  shape of `raise(event, data)`, and typed as `send()` is.
- **B. A factory of named effects, `effects(type)`.** `const pay =
  effects('payments')` answers an object whose properties make sends:
  `t.perform(pay.charge(data))`. It can carry a map of effect names to data
  types of its own, but it needs a second new verb (`perform`) to put its
  result into a block, and a Proxy or a declared list of names, and it
  hides that the result is a send.

## Decision

**Option A.** `effect(name, data?, build?)` on every action sequence
(transitions, entry and exit handlers, initial transitions, `finalize`),
beside `raise()` and `send()`:

```js
s.on('checkout', (t) => t
  .effect('charge', ({ event }) => ({ amount: event.data.total }))
  .effect('email.receipt', undefined, (send) => send.via('mail').after('1s')));
```

1. It is `send(name, block)` where the block first sets `via('effects')`
   and, when `data` is given, `data(data)`, then calls `build`. `name` is
   what `send()` takes as an event: text, a function or `named()`.
2. **The processor type is `'effects'`** unless `build` sets another with
   `via()`. `effects` is exported as `EFFECTS`, the name an application
   registers its effect processor under: `createSystem({ io: { effects:
   { send(request, context) { ... } } } })`.
3. **Types:** as `send()`: any name is accepted, and for a name of the
   chart's event map, `data` has that event's type. `build` gets the same
   `SendBuilder`.

A processor-side helper (dispatching `request.event` to a function per
effect) is not part of this: an object lookup in `send()` does it, and the
application owns its processor.

## Consequences

- A chart reads as the effects it asks for; which processor serves them
  is one registration, and a test replaces it with a recording processor.
- Nothing changes below lib: the spec, core, export and tracing see a
  `<send>`. A chart exported and imported again shows `send`, not
  `effect`.
- A chart whose system registers no `effects` processor gets what any send
  to an unknown processor gets (`error.execution`).

## Implementation notes

- **Where:** `lib/statechart.js` (`ActionSequenceBuilder.effect`,
  `EFFECTS`), `index.js` (exports `EFFECTS`), `index.d.ts`
  (`ActionSequenceBuilder.effect`, `EFFECTS`), README "Talking to the
  outside world".
- **Tests:** `test/services.js` on Node and Bun: an effect reaches the
  `effects` processor with its name and data, `build` can move it to
  another processor and give it an ID, an effect without data, and the
  exported spec is a plain send. `test/types/usage.ts`: data typed by the
  event map, `@ts-expect-error` for wrong data, and `build`'s send builder.
