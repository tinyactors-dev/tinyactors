# ADR 0004: Replace the core with the standalone runtime

**Status:** Accepted (2026-10-06). Supersedes [ADR 0003](0003-remove-tinyactors-host.md) and the ring protocol ([proposal](../proposals/ring-proto/README.md), core ADRs 0098 to 0105, napi ADR 0041).
**Date:** 2026-10-06

## Context

The ring protocol (ADR 0003) set out to fix one decision: the queue of
pending inputs lived outside core, in the host kernel. Because of it, a
send could not be refused in the macrostep that made it, overflow had to
be held somewhere, and every binding needed the same router and scheduler,
so they moved into a third layer.

Three days into its implementation, the kernel, the test host's hosted
mode and napi's native layer were deleted and napi's tests disabled; core
was between milestones M1 and M2 of four, with M3 (napi written again from
scratch) and M4 still ahead, and its last commit unfinished.

In the same days a separate repository built a runtime from scratch
around the same insight: core owns its queues. Its design
(`tinyactors-core/docs/architecture.md`):

- the system is one block of memory sized at creation; mailboxes, the run
  queue and timers are lists in a shared event pool; nothing is allocated
  after init, and the block grows by relocation when a pool is full;
- effects are pulled by the host (`ta_system_effects`); the only callbacks
  are the data model and the allocator;
- the freestanding layers (base, image, core, system) need no C library,
  checked for wasm32, x86_64, Cortex-M and RISC-V.

It already had what the ring protocol's milestones were to rebuild: all
191 automatic W3C IRP tests on an ECMAScript data model, a Node.js binding,
a WebAssembly binding, a PostgreSQL extension, hibernation, live migration
and hot reload. It was built with CMake and Python scripts.

## Decision

1. **`tinyactors-core/` is the standalone runtime.** Its tree replaces the
   earlier core; `tinyactors-test/` and `tinyactors-napi/` are removed, as
   its own C tests, conformance scenarios and bindings replace them. The
   earlier state is kept at the tag `ring-proto-final`, and the standalone
   repository's history at the tag `standalone-import`.
2. **The build is this repository's.** CMake and the Python build scripts
   are replaced by Ruby file tasks on the pinned toolchain
   (`tinyactors-core/mise/core.rb`, `mise/lib/toolchain/`). The one Python
   script left instantiates the W3C IRP templates, which are XSLT 2.0.
3. **`tinyxml` stays as it is.** `tinyactors-scxml` and `tinyactors-otlp`
   are kept but parked: they build against the earlier core, and wait to be
   ported to this one.
4. **What was not ported waits, named:** the release packaging
   (`docs/distribution.md` in tinyactors-core), the binding surface check
   (`tools/check_surface.py`), and the changelog components
   (`mise/lib/changelog/components.rb` still lists napi and the earlier
   core's API description). The [ROADMAP](../../ROADMAP.md) lists them.

## Consequences

- Core holds its queues; the questions ADR 0003 answered with rings do not
  arise. Telemetry is not built yet; the ROADMAP outlines how traces leave
  core in this design.
- A full pool stops the run (`TA_RUN_EXHAUSTED`) until the host grows the
  block; a send is not refused to the sending chart.
- Releases cannot be built until packaging is rebuilt on ADR 0001's
  machinery.
