# order

The worked example of `docs/cli.md`, "Run": an order asks the payment
processor (`payments`, an I/O processor of `config.json`) to charge its
total and waits at most 30 seconds. The first order's checkout is run with
`hold`: its charge is written and the batch held, so the order is parked
after its `<send>` and its state cannot be read (-32002). The charge is
answered with `run.send` (its `sendid` and `origintype`) before the next
`run.turn` acks the batch, and the order ends paid (`effect.log`,
`effect.done` with its donedata). The second order, spawned, sent and
turned in one JSON-RPC batch without `hold`, is never answered: its
deadline fires once `run.advanceTime` moves the clock 30 seconds, and the
forwarded `actor.state` reads it back in `open`.
