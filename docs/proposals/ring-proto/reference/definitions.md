# Definitions: one complete request, one result

**Status:** Sketch (2026-10-02), part of [the ring protocol proposal](../README.md)

A host submits a complete declarative definition through the protocol, as a
handle to a native `ta_scxml_spec` the binding built. Building the spec is
native integration, like installing the data model and value ownership.
There is no byte layout of a spec
([below](#a-native-spec-handle-not-a-byte-layout)).

## Submit a complete definition

[`h2c.definition_register`](../protocol/generated/MESSAGES.md#h2cdefinition_register)
carries a handle to one complete native spec (`spec`), a native data model
adapter handle (`datamodel`) and a diagnostic output buffer (`diagnostic`).
Core compiles and registers the spec, then writes
[`c2h.definition_result`](../protocol/generated/MESSAGES.md#c2hdefinition_result)
with the request's `seq` as `cause_seq`. Success returns a system-local
definition ID in `definition`. There are no add-state, add-transition or
finish-builder records, and no partly constructed definition for the host to
manage.

The declarative `ta_scxml_spec` and core's compiler are the semantic
implementation, as today. The binding builds the spec with core's spec
builder (`tree.c`), in its own process, before it posts the request. Builder
steps are not ring records. SCXML and language-specific front ends produce
the spec; parsers and codecs stay outside core. A host that has a chart as
text uses a chart codec on its side (ADR 0053, `ta_definition_import`) and
passes the resulting spec handle. The spec carries optional explicit
identity; omitted identity uses the existing fingerprinting rules.
Versioning and nested definitions are whatever `ta_scxml_spec` does.

Host expressions can name process-local handles installed by the binding.
The data model adapter handle names a native-installed adapter; it does not
serialize a vtable. Successful compilation copies retained spec data and
takes the native holds needed for expressions and values, under their
existing ownership rules. A failed request leaves no registration,
registration hold or retained spec data. Registration keeps ADR 0077's
identity, mismatch, adapter and host-expression semantics, including taking
one hold when it returns an existing definition.

[`h2c.definition_release`](../protocol/generated/MESSAGES.md#h2cdefinition_release)
releases one registration hold.
[`h2c.definition_retire`](../protocol/generated/MESSAGES.md#h2cdefinition_retire)
ends availability for new actors and drops registration holds. Existing
actors and nested references keep the definition alive until they finish.
Both receive the same correlated result. They are host operations, not a
second native definition-management interface. Further definition reads,
export and enumeration use request/result records too; this sketch does not
specify their shapes. Debugger commands use native primitives under
exclusive system ownership, outside the production protocol
([debugging.md](debugging.md)).

## A native spec handle, not a byte layout

`spec` is a process-local handle, like value references and the data model
adapter handle. The protocol defines no flat byte layout of `ta_scxml_spec`
(offsets, counted lists, versioning, nested definitions), for three reasons:

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
reports. Both stay valid until the correlated result is committed, even
after core releases the h2c record. The host may free or reuse them after
it reads that result. If the system is destroyed first, destruction releases
pending requests before it returns, so the host may reclaim them then. Rings
contain only handles and bounded fixed fields, even for a large spec.

The diagnostic is ADR 0058's native compile-error struct (`ta_compile_error`):
code, path of member/index steps, subject, subject byte range, related text
and message. Core writes it into the memory the host provides. No pointer
into the spec or core escapes. It reports the first fault in compile order.
`diagnostic_bytes` tells the host how much was written; it is 0 without a
compile error.

The capacity of the writable buffer bounds diagnostic output; existing
compile scratch and allocation bounds still apply. Message and
formatted-text parameters do not bound the whole path or authored subject.
A diagnostic that does not fit completely gives `capacity` with
`diagnostic_bytes = 0`; core never publishes a partial diagnostic. The host
can retry with a larger configured buffer.

`status` takes these `definition_status` values:

| Status          | When                                                                     |
|-----------------|--------------------------------------------------------------------------|
| `ok`            | the request succeeded                                                    |
| `compile_error` | the compiler's own validation refused the spec; the diagnostic says why  |
| `capacity`      | allocation, definition capacity or the diagnostic buffer was exhausted   |
| `mismatch`      | an explicit identity names different content (ADR 0077)                  |
| `not_found`     | an unknown or stale definition ID                                        |
| `invalid`       | a malformed or unknown spec or adapter handle, or a bad release request  |

No definition is registered on any failure.

## Scheduling and backpressure

Definition requests are processed in h2c order at a boundary between
macrosteps. Compilation may allocate, as it does today; it never runs inside
a macrostep and uses no event envelope. The admission watermark applies to
events, not to these requests. A definition request does not bypass earlier
h2c records blocked on admission. A host therefore submits startup
definitions before it spawns actors, and waits for their success results.

Core reserves space for the fixed `c2h.definition_result` before it consumes
the request, compiles or changes any definition state. If space is absent,
`ta_run` returns for the host to drain c2h. Every consumed request completes
with exactly one result; no macrostep end report follows. Failure is a reply
the host can act on, not an SCXML event for a nonexistent actor.

## Startup example

1. Native calls create the system, expose its rings and install the binding's
   data model and value integrations.
2. The host builds both chart specs with core's spec builder and posts two
   `h2c.definition_register` requests with their spec handles and distinct
   diagnostic buffers.
3. The host drives `ta_run`, consumes each `c2h.definition_result` and
   reclaims that request's spec and buffer. It handles errors before it
   posts spawns.
4. The host posts `h2c.spawn` using the successful definitions' identities.

The protocol is process-local: the binding resolves spec handles, blobs,
adapter handles, host-expression handles and value references. A test host
that forwards requests from a client maps these handles in its own process.
