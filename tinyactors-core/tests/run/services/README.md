# services

The client runs every service a chart invokes. Each order of `order.scxml`
invokes a `payment` service while it pays; the three orders' invocations
come as `effect.invoke`, each with its invoker, its segment and its token
(`invocation`) and the invoke's params as `data`. As the payment service
the client authorizes the first order (`run.send` with `invocation`), and
the order's `<send target="#_payment">` comes back as an `effect.send` to
the service, held with `hold`; the client ends the invocation with
`run.send` and `done` (core's `done.invoke.payment`, its data the
receipt), and the order ends paid. It fails the second order's invocation
(`run.fail` with `invocation`): the order gets `error.communication`, goes
back to `open`, and its invocation is cancelled (`effect.uninvoke`). The
third order is cancelled by an event, which cancels its invocation too.
Refused: an event from an invocation that ended (-32001 at
`params.invocation`), `done` without an invocation, `done` with an event
of its own (-32602), and a `run.sendMany` whose second event names no
session: nothing of it is sent.
