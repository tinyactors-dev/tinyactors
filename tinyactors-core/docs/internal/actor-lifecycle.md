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

An unloaded actor remains logically alive. External infrastructure may queue an event for it, or a due delayed send may make it relevant; the host/runtime can load it before resuming.

Names remain reserved while an actor is unloaded. Destroy/removal releases the name.
