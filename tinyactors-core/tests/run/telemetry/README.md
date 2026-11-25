# telemetry

The worked example's orders with an encoder (`config.json`'s `telemetry`,
a flush interval of 5 s, `service.name` `orders`), dated from
`--wall-start` (2025-11-25 07:00 UTC), so spans carry real-looking times.
The config turns log records off (`"logs": false`): `effect.logs` and
`run.metrics` write OTLP's bytes, which name the library's version, so a
transcript holding them would change with every release; `test_tool`
(`test_run_telemetry_bytes`) and `test_run_memory` read those instead.

- `order-1` checks out with `hold`. While its charge is held,
  `run.traces`, `run.metrics` and `run.post` are refused (-32002). The
  charge is answered and `order-1` ends paid; the turn's `nextDeadline`
  is the flush, 5 s on.
- At 5 s the staged spans are due: the turn's `TRACES` effect (batch 3,
  index 0) is taken and decoded by core, and `effect.traces` comes with
  that batch and index once the decode's reply does, after the ack. The
  take moves the deadline, so the turn runs once more and answers
  `nextDeadline` `null`.
- A forwarded `trace.watch` traces the order chart with `decisions` for
  60 s. `order-2` checks out (its charge not held, never answered);
  `run.traces` takes its spans now, and a second `run.traces` finds none.
- `run.post`, a message in CBOR: `actor.context` of `order-2` with the
  client's id `"ctx-2"` answers, in CBOR, `total` 990, `charge` null and
  `coupon` undefined (a `<data>` with no value, which JSON would write as
  null), the id given back; an `actor.send` notification with a byte
  string in its data (a voucher) is applied and answered with no reply
  (`coupon.applied` has no transition: `order-2` stays paying); a message
  that is not a request (an array) and one that is not one CBOR item are
  posted as they are, and core's replies, their id null, are the answers.
- At 35 s `order-2`'s payment deadline fires. `order-3` is spawned, the
  host snapshotted to `orders.snapshot` and restored from it: before the
  restore's response, the old host's open spans are ended (`trace.end`)
  and the spans it still staged (`order-2`'s timeout, `order-3`'s spawn)
  written as `effect.traces`.
- `order-3` checks out on the restored host. At the end of input the
  server posts `trace.end` and writes what is left: `order-3`'s spans,
  among them its bootstrap again, which the snapshot held unsent (same
  span ids as before the restore).

Two of core's rules show here. A record written between runs (a spawn's)
is stamped with the time of the last run before it that did work:
`order-2`'s spawn reads 0 s, since the turn at 5 s ran nothing. And
`order-1`'s macrosteps are stamped when their runs were, not stretched to
the next advance, since the server advances the clocks after each run
when it has an encoder.
