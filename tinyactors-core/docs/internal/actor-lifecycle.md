# Actor lifecycle

Conceptual lifecycle:

```text
spawn -> initializing/running -> stable
                    ^            |
                    |            v
                 paused <----- resume
                    |
                    v
                 stable
                    |
          +---------+---------+
          |                   |
        unload              terminal final
          |                   |
         load               destroy/reclaim
          |                   |
        stable              removed
```

A top-level final state terminates statechart processing. It is not necessarily identical to immediate memory reclamation: the host may need final data, and a child actor must cause `done.invoke.<id>` to reach its parent on normal completion.

The terminal actor remains resident with `TA_ACTOR_DONE` residency.
`ta_actor_get_completion()` retains the optional `<donedata>` value for the host.
An invoked child retains a pending completion while its parent is busy;
`ta_actor_publish_completion()` retries publication, and destruction is busy
while the parent invocation remains active. If that invocation was cancelled,
publication returns `TA_ERR_NOT_FOUND`, retires the pending completion, and
allows reclamation without manufacturing a done event. This preserves the
SCXML distinction between normal completion and cancellation.

Named replacement has a separate unpublished preparation phase. Preparation
uses the incumbent logical self-address and holds effects behind an activation
barrier. Activation requires a stable actor, swaps the directory entry
atomically, and then exposes the prepared effects. The incumbent is reclaimed
only by a later explicit destroy.

An unloaded actor remains logically alive. External infrastructure may queue an event for it, or a due delayed send may make it relevant; the host/runtime can load it before resuming.

Names remain reserved while an actor is unloaded. Destroy/removal releases the name.

## Passivation protocol

The host externally synchronizes routing for the logical address, captures the
actor, keeps the image for the life of the process (passivation bounds memory;
it is not persistence), and calls `ta_actor_unload()` with that
exact image. The call verifies the captured session epoch and atomically swaps
the directory entry from `RESIDENT(old_id)` to `UNLOADED`. Only after the swap
succeeds does core discard resident state. The image remains caller-owned.

Loading reverses the transition. The host resolves the data-model binding and
image, then calls `ta_actor_load()`. Core prepares the complete resident session
off-route and atomically swaps `UNLOADED` to `RESIDENT(new_id)` only when no
fallible reconstruction work remains. A failed load leaves the logical actor
unloaded and retryable.

The host must keep new inbound events fenced or queued across both operations.
Tinyactors provides no external mailbox and does not acquire the host's address
lock.

Termination also respects residency. `ta_actor_destroy()` removes a resident
actor using an expected `RESIDENT(id)` directory entry.
`ta_actor_remove_unloaded()` removes an expected `UNLOADED` entry; after it
succeeds, the host deletes the image it kept, the binding record, and the
deadline index entry.
