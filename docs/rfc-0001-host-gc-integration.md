# RFC 0001: Host GC integration hooks

Status: draft. Not accepted. No implementation task exists.
Author: design discussion, 2026-09-14.
Affects: `src/tinyactors.h` (`ta_system_config_t`, `ta_value_ops_t`), `bindings/*`.

Per `docs/implementation.md`, the public contract is not changed to make an
implementation task easier. This document proposes contract changes for the
integration owner to accept or reject. It is not authority to edit the header.

## Summary

Six optional additions that let a garbage-collected host language integrate with
the core without reimplementing root tracking, safe-point discovery and ordered
teardown in every binding. All are backward compatible. A byte-storage host
(`TA_DATA_BYTES`) sets none of them and observes no change.

## Motivation

The core is designed for a host that does manual, deterministic, ordered,
failure-aware resource management. It also hosts a garbage-collected language's
objects as actor state, through `ta_value_ops_t` (`src/tinyactors.h:1122`). Those
two requirements meet in that one struct, and every difficulty below radiates
from it.

The repository contains two bindings against the same header, which makes the
cost measurable rather than speculative.

| Concern | `bindings/ruby` | `bindings/napi` |
| --- | --- | --- |
| Value ownership layer | `ext/tinyactors_raw/raw_value.c`, 809 lines, incomplete | `native/bridge.cc:475-510`, ~35 lines |
| Root visibility | Hand-built registry, dual `wrapper_refs`/`core_refs` accounting | `napi_ref` refcount |
| Moving collector | Monotonic handle table plus `rb_gc_location` fixups | Provided by V8 handles |
| Deferred cleanup | `tar_deferred_cleanup_drain` / `_boundary` in `support.h` | `finalizeResource` defers; teardown in env cleanup hook |
| Teardown order | `tar_handle.owner` strong edges | `owner` property edge, plus kind-ordered walk at `bridge.cc:61-82` |
| Reentrancy guard | `tar_callback_enter` / `tar_callback_reentry` | `Guard` / `System::entered` |

The two bindings converged independently on the same four workarounds. That
convergence is the evidence that the missing pieces belong in the core rather
than in each binding.

### Third-host projection: CPython

A CPython extension changes the balance again. Its collector is non-moving and
refcount-primary, so `ta_value_t.handle` can hold a `PyObject *` directly and the
whole ops table reduces to `Py_INCREF` / `Py_DECREF`. Proposals 3 and, partly, 2
would be unnecessary there.

One CPython property makes proposal 1 mandatory rather than convenient:
`Py_DECREF` can execute arbitrary Python code. Dropping the last reference runs
`tp_dealloc`, which runs `__del__`, which can raise, release the GIL, resurrect
the object, trigger the cycle collector, or reenter the core. `ta_value_ops.release`
is called from inside core frames during message dequeue, arena close, state exit
and actor release. Every core invariant would have to hold across an arbitrary
host callout at each of those points. The binding must therefore enqueue instead
of decref, and it needs a published boundary at which to drain.

## Problem taxonomy

Eight distinct problems. The proposals address 1 through 6. Problems 7 and 8 are
not addressable by a hook and are documented as out of scope.

1. The core roots host objects that the host collector cannot trace.
2. A moving collector invalidates any handle the core stores.
3. `retain`/`release` are declared infallible, but host rooting can allocate.
4. Host finalizers must not call into the core or the host VM, yet cleanup is mandatory.
5. Resource destruction is ordered; host finalization is not.
6. Destruction is fallible (`TA_BUSY`), and a finalizer cannot report or retry.
7. Reachability of a host wrapper does not imply existence of the actor.
8. Cycles through core-held roots are invisible to the host cycle collector.

## Proposals

### 1. `ta_host_ops_t`: publish safe points

```c
typedef enum ta_safepoint {
    TA_SAFEPOINT_MICROSTEP = 0, /**< Between microsteps inside a run. */
    TA_SAFEPOINT_MACROSTEP,     /**< The actor is idle again. */
    TA_SAFEPOINT_RUN,           /**< ta_system_run is about to return. */
    TA_SAFEPOINT_TEARDOWN       /**< Last call before destroy or abandon completes. */
} ta_safepoint_t;

typedef struct ta_host_ops {
    void (*safepoint)(void *user, ta_safepoint_t point);
} ta_host_ops_t;
```

Added to `ta_system_config_t`. Contract: no core frame below the call forbids
reentry, and no core iteration is in progress. The host may allocate, run its own
finalizers, call public `ta_*` procedures other than `ta_system_run`, and raise
and clear host exceptions. It must return promptly.

Addresses problems 3 and 4. The core publishes when it is safe; it does not
manage host memory. Queue policy stays in the binding, which alone knows its cost.

Consumers: CPython drains its deferred decref queue. Ruby replaces
`tar_deferred_cleanup_boundary`. Node uses `TA_SAFEPOINT_TEARDOWN` for the
ordered walk it currently hand-rolls.

### 2. Root enumeration

```c
typedef enum ta_root_kind {
    TA_ROOT_ACTOR_STATE = 0, TA_ROOT_DATA_SLOT, TA_ROOT_EVENT,
    TA_ROOT_QUEUED_PAYLOAD, TA_ROOT_PENDING_SEND,
    TA_ROOT_ITERATION, TA_ROOT_CODE_OWNER
} ta_root_kind_t;

typedef struct ta_root_record {
    ta_root_kind_t kind;
    ta_actor_ref_t actor;   /**< Zero when not actor-scoped. */
    ta_value_t value;
} ta_root_record_t;

TA_API ta_status_t ta_system_visit_roots(const ta_system_t *system,
                                         ta_visit_t (*visit)(void *, const ta_root_record_t *),
                                         void *user);
```

Contract: allocation-free, non-mutating, invokes no value ops, callable while the
host collector runs, and valid at a safe point during a run.

Addresses problem 1. "What does the core hold?" becomes a question the host can
ask, rather than a ledger each binding must maintain in parallel. A Ruby mark
function, a CPython `tp_traverse` and a Node reachability audit all become one
call. The dual-accounting scheme that makes `raw_value.c` 809 lines stops
existing.

Reuses the `ta_visit_t` protocol already defined for `ta_system_inspect`
(`src/tinyactors.h:1585`, `:1644`).

Implementation cost: roots must be discoverable from stable structures even
mid-microstep. This is the most expensive proposal here and should be costed
before acceptance.

### 3. `relocate` on `ta_value_ops_t`

```c
typedef struct ta_value_ops {
    void (*retain)(void *user, ta_value_t value);
    void (*release)(void *user, ta_value_t value);
    ta_status_t (*snapshot)(void *user, ta_value_t value, ta_value_t *out_value);
    ta_value_t (*relocate)(void *user, ta_value_t value);  /**< Optional; moving hosts only. */
} ta_value_ops_t;

TA_API void ta_system_relocate_roots(ta_system_t *system);
```

The core walks every rooted slot, calls `relocate`, and stores the returned
handle. Ruby calls this from its compaction callback. V8 and CPython leave the
field `NULL`.

Addresses problem 2. Converts `src/tinyactors.h:137` ("Moving GCs must use stable
roots/indirection") from a host obligation into an optional core service, and
removes the handle-indirection table from the Ruby binding.

Depends on proposal 2's traversal machinery. Accept 2 first or together.

### 4. `ta_system_abandon`

```c
TA_API void ta_system_abandon(ta_system_t *system);
```

Unconditional teardown. Stops every actor without running exits, completes every
taken lease as `TA_CANCELLED` through its own completion path, releases every
root, drops registry-held definitions, invalidates every reference. Never fails
and returns no status.

Documented as a last-resort path for interpreter shutdown and finalizers. It is
not the graceful contract and its observable behavior is not the shutdown
contract of `ta_system_shutdown`.

Addresses problems 5 and 6. `ta_system_destroy` returns `TA_BUSY`
(`src/tinyactors.h:2042-2044`), which a `tp_dealloc`, a Ruby free function or a
Node env cleanup hook cannot report or retry. Every binding will approximate this
procedure, and each approximation can still fail.

Should be paired with an explicit statement of how a host reaches destroyability:
what must be drained after `ta_system_shutdown`, and what observable predicate
establishes quiescence. All bindings currently guess at this.

### 5. Core-enforced reentrancy

Semantics change, not a hook. `src/tinyactors.h:47-50` makes non-reentrancy a
host obligation whose violation is undefined. Proposed:

> Any public call on a system that is inside its own callback returns `TA_BUSY`,
> except `ta_context_*` operations and operations on an explicitly supplied arena.

One flag in the core converts a class of memory-safety bugs into a status code
across every binding, and retires the guard each binding maintains today. Host
language users reach this case easily: a single stray send from inside an action
callback.

### 6. Snapshot context

```c
typedef enum ta_snapshot_reason {
    TA_SNAPSHOT_SEND = 0, TA_SNAPSHOT_DELAY,
    TA_SNAPSHOT_INVOKE_ARG, TA_SNAPSHOT_ITERATION
} ta_snapshot_reason_t;

typedef struct ta_snapshot_request {
    ta_value_t value;
    ta_snapshot_reason_t reason;
    ta_actor_ref_t destination;  /**< Zero when unresolved. */
} ta_snapshot_request_t;

ta_status_t (*snapshot)(void *user, const ta_snapshot_request_t *request,
                        ta_value_t *out_value);
```

Three arguments, consistent with the header's limit. Lets a host skip a deep copy
when it can establish isolation from the reason and destination.

Paired clarification, which costs nothing and is the larger win: line 1116 states
that "Retaining the same mutable object is insufficient," implying that retaining
an immutable one is sufficient. The header never says so directly. Stating it
explicitly lets a host treat immutable values as a plain retain. Under CPython
that is the difference between `Py_INCREF` and `copy.deepcopy` on every native
send; `snapshot` runs on every native send and before every delay
(`src/tinyactors.h:1114-1121`).

## Out of scope

- **Problem 7, reachability and existence.** `ta_actor_ref_t` not retaining an
  incarnation is correct. A retaining reference would let a leaked host wrapper
  pin a terminal actor indefinitely. Bindings continue to surface `TA_STALE`.
- **Problem 8, cycles through core-held roots.** The core cannot know the host
  object graph. Proposal 2 makes the edges visible to a host cycle collector,
  which is the available limit.
- **Finalization order.** Proposal 4 makes it irrelevant rather than solving it.
  No C interface can impose order on a tracing collector.
- **Host async integration.** Driving a system from an event loop, and mapping
  `ta_io_next` onto host tasks, is a binding design problem. The core's rule that
  asynchronous work is expressed as sends and later events
  (`src/tinyactors.h:1210`) stands unchanged.

## Compatibility

| Change | Kind | Effect on an existing C host |
| --- | --- | --- |
| `ta_host_ops_t` in config | New field | None; zero means absent |
| `ta_system_visit_roots` | New procedure | None |
| `ta_value_ops.relocate` | New field | None; `NULL` means non-moving |
| `ta_system_relocate_roots` | New procedure | None |
| `ta_system_abandon` | New procedure | None |
| Reentrancy enforcement | Behavior | Previously undefined behavior becomes `TA_BUSY` |
| `snapshot` request struct | Signature | Source break for native hosts; requires ABI version bump |

Proposal 6 is the only source-breaking item. It can be deferred, or added as a
separate optional `snapshot_ex` field, if breaking native hosts is unacceptable.

Total surface: one new ops table, one new field on an existing ops table, three
new procedures, one clarification, one enforcement change.

## Open questions

1. What does root enumeration cost in the interpreter's hot structures, and is a
   safe point during a run a sufficient guarantee for proposal 2?
2. Should the deferred-release queue live in the core behind a policy flag
   instead of in each binding? The core has arenas; the argument against is that
   queue exhaustion would make an infallible operation fallible.
3. Does `TA_SAFEPOINT_MICROSTEP` fire often enough to be a throughput problem,
   and should the host declare which safe points it wants?
4. Does proposal 4 need a lease-abandon callback, or is completing each taken
   request as `TA_CANCELLED` through its existing path sufficient?
5. Should `ta_system_shutdown` gain a documented quiescence predicate as part of
   proposal 4, or separately?

## References

- `src/tinyactors.h:44-62` — threading, serialization and reentrancy contract
- `src/tinyactors.h:132-139` — `ta_value_t`, handle representation, moving collectors
- `src/tinyactors.h:1105-1126` — `ta_value_ops_t` and its contract
- `src/tinyactors.h:2042-2049` — `ta_system_destroy` and `TA_BUSY`
- `bindings/ruby/ext/tinyactors_raw/raw_value.c` — handle table, dual accounting, compaction
- `bindings/ruby/ext/tinyactors_raw/support.h` — lifetime tokens, owner edges, deferred cleanup
- `bindings/napi/native/bridge.cc:61-82,455-510` — env cleanup walk, value roots
- `bindings/napi/CONTRACT.md` — JS ownership and GC rules
