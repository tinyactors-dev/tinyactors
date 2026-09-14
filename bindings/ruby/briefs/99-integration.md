# Integration: sender-aware Ruby acceptance suite

Owner: final integration task. Status: planned.
Dependencies: all class briefs and infrastructure briefs.
Read [CONTRACT.md](CONTRACT.md) first.

## Owned files

- Central native registration and require lists
- `test/integration/*`
- `examples/request_reply.rb`
- `README.md`
- Final coverage evidence and root Makefile integration

## Required scenarios

1. A sends to B by instance, name, and parsed session URI.
2. B's guard, exit, transition, and entry callbacks see A's origin.
3. B replies through Context#reply and a declarative Send.reply action.
4. A receives B's origin with a distinct message ID.
5. Two senders receive their own delayed replies after intervening messages.
6. Name replacement does not redirect a queued or delayed reply.
7. Sender release preserves readable origin metadata and yields error.communication on reply.
8. Host, internal, and platform events have no fabricated actor origin.
9. A missing-origin reply fails explicitly instead of sending to self.
10. Eventless transitions retain the last event. Internal dequeue replaces that event.
11. Autoforward preserves the original sender. Explicit send identifies the forwarding actor.
12. Guards can inspect origin but cannot send, raise, or cancel.
13. Full queues preserve the failure status and produce one error event.
14. Expired callback contexts and memory reject use after success and exception exits.
15. GC and compaction preserve values, descriptors, snapshots, and pending I/O leases.
16. START/CANCEL races complete each request once without accepting late invocation results.
17. Upgrade preserves actor identity and rolls back failed migration.
18. Shutdown rejects destruction until actors, definitions, invocations, and leases permit it.

Run each applicable scenario through Raw and the convenience layer.
Do not use a host-posted ring as evidence of actor-originated messaging.
Verify there is no second Ruby scheduler or mailbox implementation.
Check record coverage against the current header before declaring completion.

## Completion

Build the extension from a clean checkout.
Run all Ruby tests and `make check`.
Run `ruby tools/check_ruby_interface.rb --require-complete`.
Replace planned documentation examples with tested examples.
Report runtime versions, test counts, commands, and remaining limitations.
Do not mark incomplete API entries implemented to satisfy the coverage check.
