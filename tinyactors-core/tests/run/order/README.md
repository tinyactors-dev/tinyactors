# order

The worked example of `docs/cli.md`, "Run": an order asks the payment
processor (`payments`, an I/O processor of `config.json`) to charge its
total and waits at most 30 seconds. The first order's charge is answered
with `run.send` (its `sendid` and `origintype`), and the order ends paid
(`effect.log`, `effect.done` with its donedata). The second order, spawned,
sent and turned in one JSON-RPC batch, is never answered: its deadline
fires once `run.advanceTime` moves the clock 30 seconds, and the forwarded
`actor.state` reads it back in `open`.
