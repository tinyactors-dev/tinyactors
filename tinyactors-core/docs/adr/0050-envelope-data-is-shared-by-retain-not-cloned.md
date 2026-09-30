# ADR 0050: Envelope data is shared by retain, not cloned

**Status:** Accepted; refines ADR 0006, applies ADR 0033; the open question about `ta_value_clone` and `ta_event_clone` was settled on 2026-09-23 by removing them; refined by ADR 0097: a value leaving a session is first copied by the data model's `copy_out`, then shared by retain
**Date:** 2026-09-20

## Context

ADR 0006 gives `ta_value` a vtable with `retain`, `release`, and `clone`, and
leaves the lifetime of the referent entirely to the host. Core took
co-ownership of host values in six places, and every one of them used
`ta_value_clone`, twice by way of `ta_event_clone`:

| site | what it is doing |
|---|---|
| `ta_actor_deliver` | storing the event a host delivered |
| `ta_external_delivery_clone` | a router that retains work keeps the delivery |
| `invoke.c` delayed send | the event data of a `<send>` with a delay |
| `invoke.c` completion fault | the fault data of a failed invocation |
| `ta_actor_get_completion` | handing an invoked child's data back to the host |

Every one of those wants the same thing: a reference of core's own that
outlives the caller's. Not one wants an independent copy.

Neither shipped adapter's `clone` produces one. The jq adapter's is
`jv_copy`, a reference-count bump on a persistent structure; the ECMAScript
adapter's is `JS_DupValueRT`, a reference-count bump on the same, mutable
`JSValue`. Both then allocate a fresh wrapper around the bumped handle. In
practice `clone` has always been `retain`, plus an allocation and a failure
path — and it has never delivered the isolation its name promises, so nothing
ever depended on that isolation either.

Meanwhile the one place SCXML genuinely requires a copy — `<foreach>`, which
behaves as if iterating a shallow copy — does not use `clone` at all. ADR 0033
gives it `foreach_snapshot` on the **data-model** vtable, because only the data
model knows what a shallow copy of its own collection is, and then says the
resulting snapshot is *retained* like any other value.

So the split the codebase already made, without naming it, is: copying is a
data-model operation, and `ta_value` lifetime is sharing.

## Decision

Core takes co-ownership of **envelope data** with `ta_value_retain`. Envelope
data is every host value core carries on the interpreter's behalf: event data,
a delayed send's target and data, fault data, and the completion data it hands
back.

Semantic copying remains a data-model operation. A value-level `clone` is
generic over every value a host can produce, so it cannot know shallow from
deep, which is exactly why `<foreach>` needed its own vtable entry.

`ta_value` lifetime is `retain`/`release`. A binding is complete with those
two.

## Consequences

A binding is a reference-counted wrapper and nothing else: `retain` is
`Py_INCREF` or `napi_reference_ref`, `release` is its mirror. That is the whole
value contract the future Python, Ruby, and JavaScript bindings have to meet,
and a C-native model (CBOR, say) meets it the same way the jq adapter already
does for `jv` — a small `{ references, item }` wrapper — or, for immediates,
with no-op `retain`/`release`, which ADR 0006 already anticipated.

`ta_value_retain` returns `void` and cannot fail, so two public functions lose
a failure path: `ta_actor_deliver`, and the event half of
`ta_external_delivery_clone` (whose address copy can still refuse memory, so it
keeps its `ta_result`). Fewer allocation points mean fewer out-of-memory paths
to prove.

**Sharing, not snapshotting, is now the stated rule for envelope data.** For
both shipped adapters this changes nothing, because both already shared. It is
visible only to a host whose `clone` genuinely deep-copies: such a host now
sees a queued event observe later mutation of its data. ADR 0006 leaves object
identity to the host, and `docs/internal/semantic-state.md` already requires
sharing for a session image, so this states one rule where there were two.

Both test adapters drop `clone` (`.clone = NULL`). The whole scenario suite
therefore runs on `retain`/`release` alone, which makes it the enforcement: any
future code that reaches for a clone to take ownership fails immediately and
everywhere with `TA_ERR_UNSUPPORTED`, rather than in one bespoke test that
somebody has to remember to write.

`ta_value_clone` and `ta_event_clone` remain declared. Core no longer calls
either, and no adapter in this repository implements `clone`, so both now
answer `TA_ERR_UNSUPPORTED` against the shipped adapters. Whether they stayed
in the public API was left open here, because removing them was a breaking
change.

**Settled, 2026-09-23:** `ta_value_clone`, `ta_event_clone` and the `clone`
member of `ta_value_vtable` were removed. Every caller of core is in this
repository (the root `AGENTS.md`, "The libraries are private"), and nothing
depended on them: core never called them, no adapter implemented `clone`, and
the only uses were a test asserting `TA_ERR_UNSUPPORTED` and two binding
exclusions. `retain` and `release` are now the whole value contract. A host
that wants a copy makes one in its own code.
