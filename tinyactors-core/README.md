# tinyactors (`ta`)

A portable, embeddable SCXML interpreter and cooperative actor-system VM in C.

Compile an actor system into an immutable, relocatable image; give the
library one block of mutable memory sized from that image; plug in a
datamodel; drive it with `send / run(budget) / advance_time / resume`, and
drain tagged effects.

- [docs/architecture.md](docs/architecture.md) — design, conflicts, building blocks, ABI
- [docs/concurrency.md](docs/concurrency.md) — concurrency and parallelism: what runs when,
  what you can rely on, from one actor to a PostgreSQL table of them
- [docs/scenarios.md](docs/scenarios.md) — twelve execution walkthroughs
- [docs/layout.md](docs/layout.md) — file tree, tooling, tasks
- [docs/distribution.md](docs/distribution.md) — what is released (`@tinyactors/node`, the C
  libraries), the release targets and tasks, and the plan for the web and PostgreSQL packages

The release version is `TA_VERSION_MAJOR`, `_MINOR` and `_PATCH` in
`include/tinyactors/types.h` (currently 0.2.0). The build and its tasks
are Ruby, on the repository's pinned toolchain (`mise/core.rb`,
[layout.md](docs/layout.md#tasks)):

```sh
mise install                  # at the repository root
mise run test                 # every test under ASan/UBSan, the bindings, W3C, wasm
mise run check:freestanding   # no-libc proof for wasm32 / x86_64 / Cortex-M / RISC-V
mise run check:wasm           # wasm32-wasi, tests under wasmtime
```

Status: all six layers are implemented — base, image, core,
system (freestanding: no libc/malloc/OS, checked for wasm32, x86_64,
Cortex-M and RISC-V), compile and runtime (`libtinyactors_<layer>.a`); plus the ECMAScript
datamodel on quickjs-ng (`ta_datamodel_ecmascript`, linked like a tree-sitter
language) and a BasicHTTP codec (`tinyactors/basic_http.h`). All 191 automatic W3C
SCXML IRP tests pass (`mise run w3c`). A Node.js binding with an
xstate-style API lives in [bindings/node](bindings/node) (`mise run node:test`); machines
compile to readable SCXML (written by `ta_xml_writer`, `tinyactors/compile.h`)
and all actors share one native system. The
[torture chamber](tools/torture) (`mise run torture`, `mise run torture:test`) runs
pathological scenarios — 100k actors, ~1M events/s, large datamodels, systems
addressing each other's actors, a 50,000-email campaign with one actor per
delivery attempt, how many actors fit on this machine — from a scenario home
page with a live dashboard ([design system](docs/design-system.md)), including
2 million sessions with a few thousand resident: idle actors are hibernated by
the host (`ta_actor_hibernate` / `TA_EFFECT_WAKE` / `ta_actor_wake`; the library
does no I/O). Charts **hot-reload** CGI style — new actors run the new version,
old versions drain — and running actors **migrate live** at a stable point
(`ta_actor_export` / `ta_blob_migrate` / `ta_system_adopt`), keeping their
address, data and timers and receiving `error.platform.upgrade` with their
previous bindings. [examples/access-reconciler](examples/access-reconciler) is a
Kubernetes-style controller per employee that keeps SaaS access in line with HR
(joiners, movers, leavers, approvals, drift, outages), simulated for 30 days
with invariants checked (`mise run example:access-reconciler --check`). The same JavaScript API
runs in browsers: [bindings/web](bindings/web) is the runtime compiled to
WebAssembly (~52 KB gzipped). **Links** connect systems, so browser actors talk
to server actors transparently, and the link's status is itself an actor.
Examples: [UI elements backed by actors](examples/ui-elements)
(`mise run example:ui`) and [multiplayer chess](examples/chess) (lobby, join
by code, server-hosted games; `mise run example:chess`).
See
[architecture.md §15](docs/architecture.md#15-implementation-status-and-deviations-from-this-design)
for what differs from the original design.
