# migration

Orders moved to a second version of their chart (`order-v2.scxml`, the
same chart with a `currency`, sent with the charge and named in the
donedata) while one of them is hibernated by the client.

- Three orders of version 1 (segment 0): `order-1` open, `order-2`
  hibernated (`run.hibernate`, its blob kept by the client), `order-3`
  paying, its charge sent and acked, its deadline pending.
- `run.addImage` adds version 2 as segment 1. `run.migrate` is refused
  for a chart the host does not have (-32005 at `params.chart`) and a
  session that is not there (-32001 at `params.session`).
- A migration of every `order` without `wake`, with a timeout of 10
  seconds: the next turn moves `order-1` and `order-3`
  (`effect.migrated`), and when the clock reaches the timeout
  `effect.migrationDone` reports two moved and `order-2` left pending.
- A migration of `order-2` alone with `wake`: `effect.wake` names the
  migration, the client answers with `run.wake` and the blob, and the next
  turn moves it (`effect.migrated`, then `effect.migrationDone`); with no
  actor left on it and no current chart, segment 0 retires
  (`effect.segmentFreed`).
- The migrated actors run version 2: `order-3`'s charge, answered late,
  ends it paid in EUR, `order-2`'s new charge carries the currency, and
  `order-1`'s context has the variable version 2 added. A new order is
  spawned in segment 1 (session 2^32 + 1).
- A subscription's renewal (`renewal.scxml`) invokes a payment service
  the client runs. Version 2 (`renewal-v2.scxml`) is added and the
  migration of `renewal`, with no timeout, moves it while the payment
  runs (`effect.migrated`): version 2 keeps the `<invoke>` (the same id
  and type), so the invocation goes on under its id, with no
  `effect.uninvoke`, and segment 2 retires. The client ends the payment
  (`run.send` with `done` and that id), and the renewal ends on version 2
  (its donedata names the plan).
