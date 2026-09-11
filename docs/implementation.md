# Implementation work

The public contract is `src/tinyactors.h`; semantic authority is `scxml.md`.
Do not change the public API to make an implementation task easier. Report
ambiguities to the integration owner. All functions, including private helpers,
take at most three parameters. The library is freestanding C99 and has no libc,
OS, clock, thread, allocator or transport fallback.

`tasks/tasks.sqlite3` is the implementation ledger. The root agent alone writes
it directly with sqlite3: task context, dependencies, assignments, iterations,
review findings, checks, and acceptance. A task is accepted only after independent
review; an agent reporting completion first moves it to review. Unsupported or
untested behavior is recorded, not presented as implemented.

## Module ownership

- `arena.c`: opaque linked-chunk arenas, accounting and cleanup, only public
  arena interfaces; no dependency on runtime internals.
- `internal.h`: private shared contracts; root-owned and revised at dependency
  gates. Modules request changes rather than editing each other's contracts.
- `definition.c`: immutable normalized definitions, ownership, validation and
  topology. `chart.c`: temporary nested-form lowering into those definitions.
- `system.c`: configuration, registrations, registries and system lifecycle.
- `actor.c`: actor admission, storage, identity, lifecycle and ready scheduling.
- `interpreter.c`: SCXML configuration and macrostep progression.
- `actions.c`: executable blocks, expression/value ownership and error boundaries.
- `message.c`: queues, snapshots, named routing, delayed sends and I/O leases.
- `invoke.c`, `upgrade.c`, `inspect.c`, `trace.c`: bounded integration tasks once
  their prerequisites are reviewed.

Each actor/message/definition has independently reclaimable lifetime storage.
Native roots are released exactly once. No actor scan is permitted to find ready
work or the next timer; immutable topology is shared across actors. Limits and
integer-overflow checks are part of each module, not a later integration pass.

## Acceptance

Use strict C99 desktop and ARM syntax checks, C++ public-header checks, link
symbol audits, deterministic execution traces, allocation failure injection,
sanitizers and lifecycle tests. Hosted tests may use libc; library objects may
not depend on it. Scaling claims require measurements, and ESP32 resource claims
require target measurements. A compiling example is not semantic evidence.

`compiler_runtime.c` supplies the byte primitives emitted by freestanding C
compilers for aggregate operations. These are implemented locally without libc;
an embedder with its own compiler runtime can omit that object. `make symbols`
checks undefined references across the complete archive, not one object at a time.
Hosted test executables may separately link libc for assertions and allocation.
