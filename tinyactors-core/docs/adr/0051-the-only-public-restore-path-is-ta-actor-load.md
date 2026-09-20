# ADR 0051: The only public restore path is ta_actor_load

- Status: Accepted
- Date: 2026-09-21

## Context

`ta_actor_capture()` produces a session image; two operations could turn one
back into a running session. `ta_actor_load()` restores a **named** actor as
a residency change, swapping the host directory entry from `UNLOADED` to the
new runtime ID. `ta_actor_reconstruct()` was the lower-level operation
underneath it, and was public for "anonymous sessions and tooling that does
not participate in named routing".

Restoring a session always yields a **new runtime ID**. For a named actor
that is deliberate and harmless: identity is the address, the directory
re-points, and everything that addresses the actor uses
`#_actor_<family>/<name>`, which survives. For an anonymous session there is
no address. The runtime ID *is* the identity — it is literally what a chart
sees as `#_scxml_<id>` — so a restored anonymous session is reachable by
nothing that knew the old one: a delivery the host is holding, a session URI
a chart saved from `$_event.origin`, a parent's invoke linkage.

Reusing the captured runtime ID was considered. It would defeat the purpose
of the slot/generation scheme, in which destroying an actor bumps its slot's
generation precisely so stale IDs stop resolving; a genuinely stale handle
would then resolve to the new session instead of failing. `session-image.md`
also calls the captured ID "local freshness metadata, not portable identity",
and ADR 0028 makes runtime IDs compact handles that persistence must resolve.

That leaves the question of who actually wanted it. Reviewing the candidates:

- **Invoked children** are always anonymous, but they are never restored.
  Restoration *retires* every active invocation and reports
  `error.communication.invoke.<id>.unavailable`; children are abandoned by
  design, not migrated.
- **A host doing its own routing** still needs some identity to route to, so
  it keeps a name-to-ID map and updates it on restore. That is a directory,
  hand-rolled.
- **Verification** (ADR 0020, ADR 0021) drives the production transition
  engine with fake I/O processors, exercising the real code paths rather than
  snapshotting and restoring sessions underneath it.
- **`ta_actor_load()`** needs it internally, which is a reason for the code to
  exist, not for the API to be public.

## Decision

`ta_actor_reconstruct()` and `ta_actor_reconstruct_params` move to
`src/session_image.h`. `ta_actor_load()` is the only public way to restore an
image, and a session therefore has to have a name to be restored under.

`ta_actor_capture()`, `ta_session_image`, `ta_session_image_next_deadline()`
and `ta_session_image_destroy()` stay public: unload requires a fresh image
from the caller, and a host indexes unloaded actors by their deadlines.

## Consequences

Capture stays available to anyone — for check-pointing, for reading a
deadline, for handing to `ta_actor_unload()`. What narrows is restoration,
which now always carries a name.

The residency scenarios move onto named actors and `cycle_residency` (capture,
unload, load), so they exercise the path a host actually has. Two of them —
capturing without restoring, and reading an image's deadline — need no name
and keep using anonymous actors.

One claim cannot be made any more: that restoring a session is invisible to a
host holding a delivery addressed to the old one. It never could be, for an
anonymous session, and for a named one ADR 0014 already puts fencing inbound
events across a residency change on the host.
