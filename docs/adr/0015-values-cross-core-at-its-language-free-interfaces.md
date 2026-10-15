# ADR 0015: Values cross core at its language-free interfaces; `actor.send` is a posted method

**Status:** Accepted (2026-10-15). Amends [ADR 0011](0011-the-control-plane-is-posted-messages.md)
(the control plane gains a method that sends an event) and follows
[ADR 0014](0014-other-systems-are-an-io-processor.md).
**Date:** 2026-10-15

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the operator and the observability backend**, through a client
  of the control plane that has no binding (a debugger, an admin tool, a
  test driver, the MCP-server proposal): it can read an actor's variables
  (`actor.context`) but cannot give an actor an event.
- **Then: the maintainers.** ADR 0014 removed the router, the only caller
  of the datamodel's `from_wire`, while `to_wire` kept its callers (trace
  values, `actor.context`). The pair looked broken; it is not: both are the
  value boundary of core's language-free interfaces, where a value crosses
  as a neutral CBOR value and only core knows which datamodel reads it.
- **Out of scope:** a second hot path (a program in the process calls
  `ta_host_send`), values in another system's wire format (ADR 0014).
- **Who pays:** nothing per event; per `actor.send`, one CBOR item
  re-encoded from the message and read by the datamodel; one method in
  core; a typed method in each binding.

## Decision

1. `to_wire` and `from_wire` stay a pair, described as the neutral values
   at core's language-free interfaces: out in traces and `actor.context`,
   in with `actor.send`.
2. `actor.send` is a posted method: the target by `session` or local
   `address` (core resolves it), `event`, `data` (read by the target
   session's datamodel with `from_wire`), `origin` and `sendid`; the reply
   is `{}` once the event is in the mailbox. `docs/requests.md` describes
   it; its "Not methods" list keeps `ta_host_send` as the hot path.
