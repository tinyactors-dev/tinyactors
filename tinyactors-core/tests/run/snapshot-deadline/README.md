# snapshot-deadline

An order's payment deadline (a delayed send, 30 s, the chart of `order`)
carried by a snapshot and fired after a restore.

- `order-17` checks out at 0: its charge is sent and acked, its deadline
  due at 30 s. At 10 s `run.snapshot` writes `orders.snapshot`.
- The original host runs on to 30 s and the deadline fires: the order is
  back in `open`.
- `run.restore` from the file puts the clock back at 10 s. `actor.get`
  lists the pending deadline, due at 30000; the next turn answers it as
  `nextDeadline`, nothing fires at 29 s, and at 30 s the deadline fires
  with the same lines as on the original host.
- A second `run.restore` from the same file: this time the charge is
  answered before the deadline, the order ends paid, its `<onexit>`
  cancels the deadline, and at 30 s nothing fires (`nextDeadline` `null`).
