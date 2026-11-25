# snapshot-services

Invoked services across a snapshot: an order paying through a payment
service the client runs (the chart of `services`) and a subscription's
renewal (the chart of `migration`), both running when the host is
snapshotted.

- `order-1` checks out and `renewal-1` starts: two `effect.invoke`s.
- Version 2 of `renewal` is added and a migration of `renewal` started.
  `run.snapshot` is refused while it runs (-32002, until its
  `effect.migrationDone`). The next turn ends it: `renewal-1` cannot move
  while its invocation runs (`failed`, `TA_ERROR_STATE`). Then the
  snapshot is written.
- The original host goes on: the payment is authorized, captured and done,
  and `order-1` ends paid.
- `run.restore` from the file: segments 0 and 1. The first turn announces
  both invocations again, `effect.invoke` with `restored`, their tokens,
  types, sources and params as before. This time the client fails the
  order's payment (`run.fail` with `invocation`): the invocation is
  cancelled (`effect.uninvoke`) and the order is back in `open`. The
  renewal's payment is done, and `renewal-1` ends on version 1.
- A renewal spawned after the restore runs version 2, in segment 1.
