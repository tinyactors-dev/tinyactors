# Definitions: one complete request, one result

**Status:** Sketch (2026-10-02), part of [the ring protocol proposal](../README.md)

**Accepted boundary:** hosts submit complete declarative definitions through
the protocol, as a handle to a native `ta_scxml_spec` the binding built.
Building the spec, like installing the data model and value ownership, is
native integration. There is no flat byte layout of a spec
([below](#decided-a-native-spec-handle-not-a-byte-layout)).

## Submit a complete definition

[`h2c.definition_register`](../protocol/generated/MESSAGES.md#h2cdefinition_register)
contains a handle to one complete native spec, a native data model adapter
handle and a diagnostic output buffer. Core compiles and registers it, then writes
[`c2h.definition_result`](../protocol/generated/MESSAGES.md#c2hdefinition_result)
with the request's `seq`. Success returns a system-local definition ID.
There are no add-state, add-transition or finish-builder records, and no
partly constructed definition for the host to manage.

The existing declarative `ta_scxml_spec` and compiler remain the semantic
implementation. The binding builds the spec with core's existing spec
builder (`tree.c`), in its own process, before it posts the request. SCXML
and language-specific front ends produce that spec; parsers and codecs stay
outside core. A host that has a chart as text uses a chart codec (ADR 0053,
`ta_definition_import`) on its side and passes the resulting spec handle.
The spec carries optional explicit identity; omitted identity uses the
existing fingerprinting rules. Versioning and nested definitions are
whatever `ta_scxml_spec` does. Builder steps stay off the rings.

Host expressions can name process-local handles installed by the binding.
The data model adapter handle also names a native-installed adapter; it
does not serialize a vtable. Successful compilation copies retained spec
data and takes the native holds needed for expressions and values, under
their existing ownership rules. A failed request leaves no registration,
registration hold or retained spec data. Registration keeps ADR 0077's
identity, mismatch, adapter and host-expression semantics, including taking
one hold when returning an existing definition.

[`h2c.definition_release`](../protocol/generated/MESSAGES.md#h2cdefinition_release)
releases one registration hold.
[`h2c.definition_retire`](../protocol/generated/MESSAGES.md#h2cdefinition_retire)
ends availability for new actors and drops registration holds. Existing
actors and nested references keep the definition alive until they finish.
Both receive the same correlated result. These are host operations, rather
than a second native definition-management interface. Further definition
reads, export and enumeration should use request/result records too; their
record shapes are not yet specified. Debugger commands use native primitives
under exclusive system ownership, outside the production protocol
([debugging.md](debugging.md)).

## Decided: a native spec handle, not a byte layout

`spec` is a process-local handle, like value references and the data model
adapter handle. A flat byte layout of `ta_scxml_spec` (offsets, counted
lists, versioning, nested definitions) is rejected:

- **Every host builds specs natively.** napi and the test host do it in C.
  A JavaScript host on wasm calls core's exported builder functions, as
  napi does.
- **The protocol is process-local.** A handle never leaves the process, so
  nothing needs a portable encoding.
- **A layout would duplicate `ta_scxml_spec`** field for field, and both
  would have to change together.

A malformed native spec is what a native caller gets today: a bad pointer
is a crash, not `invalid`.

## Handles, buffers, bounds and errors

The host owns the spec and the diagnostic buffer. `spec` names a native
spec; `diagnostic` names writable memory whose capacity the byte sink
reports. Both remain valid until the correlated result is committed, even
after core releases the h2c record. The host may free or reuse them after
reading that result. If the system is destroyed first, destruction releases
pending requests before returning, so the host may reclaim them then. Rings
contain only handles and bounded fixed fields, even for a large spec.

The diagnostic is ADR 0058's native compile-error struct: code, path of
member/index steps, subject, subject byte range, related text and message.
Core writes it into the memory the host provides. It reports the first
fault in compile order. `diagnostic_bytes` tells the host how much was
written. The writable buffer's capacity bounds diagnostic output; existing compile
scratch and allocation bounds still apply. Message and formatted-text
parameters do not bound the whole path or authored subject. If the output
buffer cannot hold the complete diagnostic, core returns `capacity`
with `diagnostic_bytes = 0`; it never publishes a partial diagnostic. The
host can retry with a larger configured buffer. Allocation or
definition-capacity exhaustion also returns `capacity`; no definition is
registered on any failure. A malformed or unknown spec or adapter handle
returns `invalid`; a spec the compiler's own validation refuses returns
`compile_error` with its diagnostic; an identity/content conflict returns `mismatch`.

## Scheduling and backpressure

Definition requests are processed in h2c order at a boundary between
macrosteps. Compilation may allocate, as it does today; it never runs inside
a macrostep and uses no event envelope. The admission watermark applies to
events, not these requests. A definition request does not bypass earlier
h2c records blocked on admission. A host therefore submits startup
definitions before spawning actors, and waits for their success results.

Core reserves space for the fixed `c2h.definition_result` before consuming
the request, compiling or changing any definition state. If space is absent,
`ta_run` returns for the host to drain c2h. Every consumed request completes
with exactly one result; no macrostep end report follows. Failure is a reply the
host can act on, rather than an SCXML event for a nonexistent actor.

## Startup example

1. Native calls create the system, expose its rings and install the binding's
   data model and value integrations.
2. The host builds both chart specs with core's spec builder and posts two
   `h2c.definition_register` requests with their spec handles and distinct
   diagnostic buffers.
3. The host drives `ta_run`, consumes each `c2h.definition_result` and
   reclaims that request's spec and buffer. It handles errors before posting spawns.
4. The host posts `h2c.spawn` using the successful definitions' identities.

The protocol remains process-local: spec handles, blobs, adapter handles, host-expression
handles and value references are resolved by the binding. A test host that
forwards requests from a client maps these handles in its own process.
