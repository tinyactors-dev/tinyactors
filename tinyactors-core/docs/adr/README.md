# Architecture decision records

ADRs are the primary record of TA's design intent. Unless marked otherwise, the decisions below are **Accepted**.

| ADR | Decision |
|---|---|
| 0001 | SCXML semantics without an XML dependency |
| 0002 | In-process library; embedder owns concurrency and I/O environment |
| 0003 | Immutable shared definitions and per-actor sessions |
| 0004 | Named definitions; named and anonymous actors |
| 0005 | Pluggable data models through fat-pointer vtables |
| 0006 | Host-defined values own lifetime and serialization |
| 0007 | Structured interned event names and prefix matching |
| 0008 | `error.*` events are control flow; C errors are separate |
| 0009 | Semantic run-to-completion with pausable/resumable execution |
| 0010 | Parallel configuration is a set of active atomic states |
| 0011 | The host owns external scheduling; actor owns internal events |
| 0012 | Logical time and engine-owned delayed-send semantics |
| 0013 | Snapshot the complete running session |
| 0014 | Unload is residency change; destroy is termination |
| 0015 | Synchronous I/O callbacks, asynchronous completion by events |
| 0016 | Invocation is a pluggable service boundary |
| 0017 | No implicit thread safety; external synchronization |
| 0018 | Optimize memory per actor; share everything definition-level |
| 0019 | Definitions are built/compiled to compact immutable layouts |
| 0020 | Verification reuses the production transition engine |
| 0021 | Verification begins as simulation/fuzzing, not proof |
| 0022 | Actor event origin is the implicit reply address |
| 0023 | Allocator injection and freestanding-friendly core |
| 0024 | C ABI targets Node, CPython, CRuby, WASM, and embedded hosts |
| 0025 | SCXML import/export is optional and separate |
| 0026 | Delayed sends are semantically actor-owned, physically globally indexed |
| 0027 | Detailed generated event names remain SCXML-prefix-compatible |
| 0028 | Runtime IDs are compact handles; persistence must resolve logical identity |
| 0029 | No per-actor external mailbox is required by the core |
| 0030 | Interpreter extensions cannot bypass statechart configuration semantics |

See `../internal/api-gaps.md` for places where the current header skeleton does not yet fully expose these decisions.
