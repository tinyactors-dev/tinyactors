# hold

Answering sends with `hold`: three orders (`order.scxml`, the worked
example's chart) are spawned in a batch and checked out with one
`run.sendMany`; `run.turn` with `hold` writes their three charges and
answers `held`, the batch left open. While it is held, a forwarded
`actor.state` and `run.addImage` are refused (-32002 `state`), and the
lookups answer (`run.chart`, `run.actorByName`, `run.resolve`). The client
answers the first charge with `run.send` and fails the second with
`run.fail`; the next `run.turn` acks the batch, and the orders go on with
the answer and the `error.communication` already in their queues: the
first ends paid, the second goes back to `open`. The third is never
answered: its 30 second deadline fires once the clock moves. `run.ack`
with no batch held is refused. A second order's charge, held and then
acked with `run.ack` without an answer, waits for its deadline;
`run.terminate` ends it and `run.actorByName` then finds no actor of its
name. The session ends with a fourth order's charge held: the end of
input acks it and the server exits 0.
