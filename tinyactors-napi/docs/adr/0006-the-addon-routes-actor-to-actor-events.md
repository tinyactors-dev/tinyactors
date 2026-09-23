# ADR 0006: The addon routes actor-to-actor events

**Status:** Accepted; addresses interned with ADR 0014
**Date:** 2026-09-23

## Context

Core hands every event between sessions to the host's external-event router
(tinyactors-core ADR 0040). The first addon never installed a router. Every
`<send>` between actors therefore became a communication error, so every
message in the system had to come from JavaScript.

## Decision

1. **Every system installs a router when it is created.** `submit`
   resolves the destination:
   - a runtime ID is used as it is;
   - a named address is resolved through the addon's actor directory.

   The router then copies the borrowed delivery into a pooled envelope and
   appends it to the target's mailbox (ADR 0005). It returns
   `TA_EXTERNAL_EVENT_ACCEPTED`. Data is shared by retain, not cloned
   (tinyactors-core ADR 0050).
   - The router does not call `ta_external_delivery_clone()`, which copies
     the strings of every named address. A named address in the origin is
     looked up in the system's **address table** instead: a map from
     `(family, name)` to one reference-counted copy of the address, made
     the first time the address is seen. The envelope holds a reference, and
     the event handed to `ta_actor_deliver()` borrows the table's strings.
   - A named origin stays a name, so a reply still follows the directory
     (tinyactors-core ADR 0022) even when the actor is replaced.
   - The directory's entries use the same table, so an address that is both
     an actor's name and a sender costs one copy.
2. **The router never delivers directly and never re-enters the
   interpreter.** Mail sent during a macrostep is delivered in a later
   step of the pump.
3. **Retry is only for resource limits.** `TA_EXTERNAL_EVENT_RETRY` is
   returned only when a backpressure limit (ADR 0005) or an allocation
   stops the router from accepting. Core keeps the delivery and offers it
   again.
4. **An unknown or finished destination is not a router error.** The
   delivery is accepted, and it becomes a dead letter:
   - a named address with no directory entry, or a runtime ID whose
     generation does not match the scheduling table, is recorded as a dead
     letter at once (ADR 0005);
   - a finished target is found when its mailbox is delivered.

   This keeps routing O(1) and gives messages the same order whether their
   target lives or dies in between.
5. **The addon provides an in-memory actor directory**
   (`ta_system_set_actor_directory`) for named actors. It is a hash map from
   an interned address to its directory entry. Persistent directories are a
   later layer.

## Consequences

- Actor-to-actor traffic stays in native code from end to end. JavaScript
  is involved only in data-model callbacks (ADR 0011) and in hooks the
  application registers.
- Mail from JavaScript and mail between actors uses one queue topology,
  with one ordering and backpressure policy.
- Actor-to-actor traffic between named actors copies no strings after the
  first message. The address table grows with the number of distinct live
  addresses, and an address is freed when its last reference is released.
