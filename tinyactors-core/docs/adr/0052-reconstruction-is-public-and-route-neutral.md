# ADR 0052: Reconstruction is public and route-neutral

- Status: Accepted; supersedes ADR 0051
- Date: 2026-09-21

## Context

ADR 0051 made `ta_actor_load()` the only public operation that could restore a
session image. That conflated two independent operations:

- reconstruction creates a resident interpreter from captured state; and
- loading changes a named actor's residency and publishes the new runtime ID.

The distinction matters during replacement. A host may capture named actor
`x`, replace its logical route with a new interpreter under a new chart
revision, and later reconstruct the captured interpreter. Reconstructing the
old session must not take the route back from the replacement. Sends to
`#_actor_<family>/x` must continue to resolve through the directory to the new
actor.

The same route-neutral primitive is useful for debugging, verification,
branching, and anonymous sessions. A reconstructed session gets a fresh
runtime ID. Stale runtime IDs remain stale; reconstruction never reuses the
captured slot or generation.

## Decision

`ta_actor_reconstruct()` and `ta_actor_reconstruct_params` are public again.
The operation reconstructs the exact captured definition revision, returns a
fresh runtime ID, and does not read, claim, replace, or remove a directory
entry.

For a named image, reconstruction preserves the logical self-address recorded
in the image. That does not publish the reconstructed session at the address.
Any send to the logical address follows the directory's current entry, which
may name a replacement actor. The reconstructed session itself is controlled
directly through its returned runtime ID.

For an anonymous image, reconstruction assigns the new runtime ID as the
session's self-reference. References to the captured runtime ID remain stale.

`ta_actor_load()` remains the public residency operation. It reconstructs
off-route, then compare-and-swaps the named directory entry from `UNLOADED` to
the new runtime ID. A failed swap destroys the unpublished reconstruction and
leaves the actor unloaded and retryable.

## Consequences

Reconstruction no longer implies routing or identity takeover. Callers that
want to resume an unloaded virtual actor use `ta_actor_load()`; callers that
want an independent resident reconstruction use `ta_actor_reconstruct()`.

More than one reconstruction can exist, and reconstructing the same image more
than once can duplicate interpreter-owned pending work. The host is responsible
for deciding when such branching is appropriate. This is already inherent in
exposing an owned, reusable session image.

Named logical identity remains separate from runtime identity. A replacement
continues to receive address-routed sends even when an older captured session
is reconstructed later.
