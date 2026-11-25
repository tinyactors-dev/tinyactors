# hibernation

Three orders (the chart of `order`) hibernated by the client, which keeps
their blobs: the server keeps none.

- `run.sweep` finds no order at rest on its first revolution (the CLOCK
  sweep marks them) and all three on its second; `run.hibernate` answers
  each blob. With no batch open, the blob opened a batch of its own, acked
  before the response, so the forwarded `actor.state` that follows is
  answered (core's refusal: the actor is hibernated). Hibernating an
  actor already hibernated is -32002.
- A `checkout` sent to the hibernated `order-1` is kept by the host, and
  the next `run.turn` asks for the actor with `effect.wake`. The client's
  `run.wake` is refused with another order's blob (-32005, at
  `params.blob`), a session that is not there (-32001, at
  `params.session`) and text that is not base64url (-32006), then taken
  with the order's own blob; the next turn runs the checkout.
- While the charge is held, `run.hibernate` of `order-3` is allowed: its
  blob joins the held batch, which the next `run.turn` acks. `order-1`,
  parked after its `<send>`, is not at rest (-32002).
- `order-2` is woken, checks out, and is hibernated with its payment
  deadline pending: the host keeps the delayed send, and when the clock
  passes it, `effect.wake` asks for the order. Its first blob is refused as
  not the latest (-32001 at `params.blob`); its latest wakes it and the
  deadline fires.
- `order-3` is woken from the blob taken while the batch was held, and is
  back in `open`. Sweeps with `limit` 1 answer one session at a time.
