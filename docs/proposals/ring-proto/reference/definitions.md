# Definitions: one complete request, one result

**Status:** Sketch (2026-10-02), part of [the ring protocol proposal](../README.md)

**Accepted boundary:** hosts submit complete declarative definitions through
the protocol. Native integration for the synchronous data model and value
ownership remains desired. The representation below is a sketch; its exact
byte layout is still open.

## Submit a complete definition

[`h2c.definition_register`](../protocol/generated/MESSAGES.md#h2cdefinition_register)
contains one complete specification, a native data model adapter handle and
a diagnostic output buffer. Core compiles and registers it, then writes
[`c2h.definition_result`](../protocol/generated/MESSAGES.md#c2hdefinition_result)
with the request's `seq`. Success returns a system-local definition ID.
There are no add-state, add-transition or finish-builder records, and no
partly constructed definition for the host to manage.

The existing declarative `ta_scxml_spec` and compiler remain the semantic
implementation. SCXML and language-specific front ends produce that spec;
parsers and codecs stay outside core. The proposed buffer representation
uses offsets and counted lists for the spec tree, authored text, expressions,
literal content and source spans. It carries the same information as the
native spec, including optional explicit identity; omitted identity uses
the existing fingerprinting rules. It contains no native pointers or
compiled runtime identifiers. Core validates offsets, lengths, tree shape
and bounds before compiling. Exact field layout, versioning and the
representation of nested definitions still need specification.

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
record shapes are not yet specified. Operator controls remain a separate,
pending decision.

## Buffers, bounds and errors

The host owns both buffers. `spec` names a readable `blob`; `diagnostic`
names a writable `blob` whose capacity the byte sink reports. They remain
valid until the correlated result is committed, even after core releases
the h2c record. The host may reuse them after reading that result. If the
system is destroyed first, destruction releases pending requests before
returning, so the host may reclaim their buffers then. Blob access is the
existing native byte source/sink integration; it does not add a chart codec
to core. Rings contain only handles and bounded fixed fields, even for a
large spec.

A compile diagnostic copies ADR 0058's code, path of member/index steps,
subject, subject byte range, related text and message into the output
buffer, using offsets and lengths rather than borrowed pointers. It reports
the first fault in compile order. `diagnostic_bytes` tells the host how much
was written. The exact diagnostic encoding is also still a sketch question.
The writable buffer's capacity bounds diagnostic output; existing compile
scratch and allocation bounds still apply. Message and formatted-text
parameters do not bound the whole path or authored subject. If the output
buffer cannot hold the complete diagnostic, core returns `capacity`
with `diagnostic_bytes = 0`; it never publishes a partial diagnostic. The
host can retry with a larger configured buffer. Allocation or
definition-capacity exhaustion also returns `capacity`; no definition is
registered on any failure. Malformed representations or adapter handles
return `invalid`; an identity/content conflict returns `mismatch`.

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
with exactly one result; no macrostep fence follows. Failure is a reply the
host can act on, rather than an SCXML event for a nonexistent actor.

## Startup example

1. Native calls create the system, expose its rings and install the binding's
   data model and value integrations.
2. The host produces both chart specs and posts two
   `h2c.definition_register` requests with distinct buffers.
3. The host drives `ta_run`, consumes each `c2h.definition_result` and
   reclaims that request's buffers. It handles errors before posting spawns.
4. The host posts `h2c.spawn` using the successful definitions' identities.

The protocol remains process-local: blobs, adapter handles, host-expression
handles and value references are resolved by the binding. A test host that
forwards requests from a client maps these handles in its own process.
