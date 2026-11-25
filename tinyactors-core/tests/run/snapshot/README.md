# snapshot

A population of orders snapshotted to a file, the original host gone on
past it, then restored in place and continued. The order chart draws a
reference with `Math.random` and stamps the checkout with `Date`, so the
restore shows quickjs's state, its random state and the wall clock coming
back; `invoice.scxml` keeps a function in a `<data>`, which no snapshot
stores.

- Three orders; `order-1`'s charge is held. While the batch is held,
  `run.snapshot` and `run.restore` are refused (-32002). The charge is
  answered and `order-1` ends paid.
- `order-2` checks out without `hold` (its 30 s payment deadline pending),
  `order-3` is hibernated (its blob, B1, kept by the client), the clock
  moves 10 s, and `run.snapshot` writes `orders.snapshot` (in the copy of
  this directory the test runs in) and answers its size.
- The original host goes on: `order-4` is spawned and draws its
  reference; `order-3` is woken from B1, checks out and is hibernated
  again (B2, taken after the snapshot); at 30 s `order-2`'s deadline
  fires.
- `run.restore` from the file: the clock is back at 10 s (its wall clock
  too), `order-2` is paying again and `order-4` is not there. Spawned
  again, `order-4` is session 4 again and draws the same reference: the
  random state came back with quickjs's.
- A charge answer for the hibernated `order-3` asks for it
  (`effect.wake`); the client answers with B2, a blob taken after the
  snapshot, which the restored host takes (it keeps the snapshot's
  identity). `order-3` is paying, as B2 has it, and ends paid with the
  stamp of its checkout. Its deadline was set after the snapshot, so the
  restored host has none for it.
- At 30 s `order-2`'s deadline fires again, after the restore.
- Refusals: a pack that is not a snapshot and a chart file (-32005 at
  `params.snapshot` and `params.path`), a file that is not there
  (-32005), text that is not base64url (-32602, the schema's pattern),
  `-` as a path (-32602).
- `invoice-1` keeps a formatter function in a `<data>`: the snapshot is
  refused with -32007, the message naming the variable. Once the invoice
  has ended, the snapshot is taken.
