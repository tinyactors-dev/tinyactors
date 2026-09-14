# Tinyactors

A freestanding C99, sans-I/O statechart actor runtime. The public contract is
[src/tinyactors.h](src/tinyactors.h). It uses SCXML semantics and terminology,
without an XML parser or a built-in scripting language.

Definitions use nested C structs, designated initializers and sentinel-terminated
state/transition arrays. Prepared revisions are immutable and shared. Actors
store either a native-language object handle or an aligned copy-safe C byte
block. All public and private C functions take at most three parameters.

The runtime includes hierarchical/parallel states, history, run-to-completion
execution, executable blocks, internal/external queues, delayed sends, named
incarnations, SCXML and external invocation, processor leases, transactional
upgrade, streaming inspection, and optional host-provided OTel span hooks.

## Build and verification

```sh
make                 # build/libtinyactors.a
make check           # tests, strict C99/C++ header, ARM syntax, symbols, arity
make ubsan           # hosted undefined-behavior checks
make benchmark       # two complete lifecycles of 200,000 entities
make sanitize        # hosted ASan + UBSan, on a working ASan platform
```

The normal archive has no unresolved runtime/libc symbols. Compiler-emitted byte
primitives are supplied in `src/compiler_runtime.c`; omit that object if the host
already supplies its compiler runtime. Hosted tests and benchmarks use libc.
`make sanitize` uses host primitives to allow sanitizer interception.

The verified desktop suite has 25 test executables. It covers ownership and
allocation failures, statechart execution, invocation cancellation, upgrades,
name replacement, delayed routing, cross-system frame export, and message spans.
See [docs/implementation.md](docs/implementation.md) for architecture and
[docs/validation.md](docs/validation.md) for measurements and validation limits.

## Embedding

Provide chunk acquire/release callbacks, then create a root arena and system.
Host language bindings supply the data-model and value-ownership callbacks;
plain C models can use byte state and custom action/guard callbacks. Compile or
build a definition, spawn actors, post messages, and call `ta_system_run` with a
monotonic time and bounded work budget.

Perform transport work outside runtime callbacks: take `ta_io_next` leases and
complete each exactly once. The host owns sockets, timer wakeups, synchronization,
serialization, GC roots, and telemetry export. No background thread or allocator,
clock, filesystem, network, VM, or OTel SDK is included. Serialize calls per system;
different systems can run independently on different host-owned threads.

Stop/release top-level actors and complete outstanding I/O before destroying the
system. Arena close alone does not replace the system shutdown protocol.

The [Node-API binding](bindings/napi/README.md) adds an exact C mapping and an
ergonomic JavaScript/TypeScript layer with native object state, nested machine
definitions, streaming inspection, I/O leases, and explicit Node scheduling.
Build and test it with `npm --prefix bindings/napi run check`.
Its [realtime chat example](bindings/napi/examples/chat/README.md) runs a real
multi-client browser chat with one directory actor, one actor per room, and
reconnecting client-session actors.

The [Ruby binding](bindings/ruby/README.md) is being rebuilt. Its previous
implementation was removed. [Class briefs](bindings/ruby/briefs/README.md)
specify procedural `TinyActors::Raw` bindings and a Ruby convenience layer.

The [WebSocket example](examples/websocket_connection.c) models one ephemeral
connection with ready/blocked states, heartbeat and timeout handling. Its host
callbacks are deliberately transport abstractions, not a networking server.

## Implementation ledger

[tasks/tasks.sqlite3](tasks/tasks.sqlite3) records task context, dependencies,
Luna assignments, root review findings, revisions, checks and results. It is
managed directly with SQLite, not generated from a transient task list.

```sh
sqlite3 tasks/tasks.sqlite3 'select id, status, result from tasks;'
sqlite3 tasks/tasks.sqlite3 'select task_id, command, exit_code, output from checks;'
```
