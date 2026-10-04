# Proposal: from scratch 1

This is a proposal for rewriting the core from scratch, derived from first principles.

## Task
You are designing the most performant, memory-conservative, portable, embeddable **SCXML interpreter and actor-system runtime in C** possible.

The implementation must support full SCXML semantics while being suitable for embedding in:

- C/C++
- Rust
- Python
- Ruby
- Java/JVM
- .NET
- Go
- Node.js
- Swift/iOS
- Kotlin/Android
- WebAssembly
- SQLite/PostgreSQL/database extensions
- constrained/embedded environments

The same library should also be capable of packaging and running **entire systems of communicating SCXML actors**, so that an ordinary host does not need to implement actor mailboxes, internal event queues, scheduling, timers, cross-instance routing, SCXML invocation, or the built-in SCXML I/O processor.

Design the architecture from first principles.

## Core architectural principle

The SCXML implementation owns **SCXML semantics**.

The host should own only things that genuinely belong to the host, such as:

- application/domain objects
- implementation of the selected SCXML datamodel
- genuinely external I/O
- integration with the host event loop
- optionally memory allocation and clock/time source

Do not push SCXML-specific bookkeeping into every language binding.

At the same time, keep the semantic kernel small enough that it can be used independently of the higher-level actor-system runtime.

Consider a layered architecture along these lines, but challenge it if appropriate:

- `ta_core`: minimal SCXML execution kernel
- `ta_system`: multi-instance actor runtime
- `ta_compile`: parser, validation, static analysis and compilation
- `ta_runtime`: optional OS/convenience integrations

## Compiled immutable representation

Do not interpret XML directly during normal execution.

SCXML should be compiled ahead of time into a compact immutable Chart IR.

Explore a representation using:

- integer IDs rather than pointers
- offsets/ranges rather than linked structures
- packed tables
- position-independent data
- mmap/ROM/flash friendliness
- sharing one chart definition among arbitrarily many instances
- precomputed state hierarchy information
- precomputed transition domains
- precompiled event matchers
- precomputed information useful for exit/entry/conflict calculations
- compact bitsets where appropriate
- flattened executable content

Ideally a chart can exist as a single relocatable binary blob.

Also consider a compiled **system image** containing multiple charts and actor/system definitions.

## Mutable instances

An SCXML instance must contain only genuinely mutable execution state.

Do not copy the chart structure into every instance.

Aim for extremely small per-instance memory.

Explore chart-dependent instance layouts where:

    ta_instance_size(chart)

determines the required memory and the embedder can supply it.

The execution core should preferably perform no mandatory heap allocations during execution.

Separate:

- persistent actor/machine state
- temporary execution workspace
- immutable chart state

## Datamodel independence

The interpreter must be genuinely SCXML-datamodel agnostic.

Do not invent a universal JSON-like value system.

A Python binding already has `PyObject *`.
Ruby has `VALUE`.
PostgreSQL has `Datum`.
A tiny embedded application may use indexes into its own arena.

The core should ask the datamodel implementation to perform the semantic operations required by SCXML.

Expressions should be represented to the core using compact expression IDs/handles. The selected datamodel decides what those IDs mean.

For example, an expression ID could map to:

- compiled Python
- JavaScript bytecode
- host-language closures
- an embedded expression VM
- a C switch statement

Design the minimum semantic datamodel interface required for full SCXML.

Pay special attention to SCXML system variables such as:

- `_event`
- `_sessionid`
- `_name`
- `_ioprocessors`

These should remain under correct SCXML semantic control rather than requiring the host application to manually synchronize them with the interpreter.

## Executable content

SCXML executable content should be compiled into a tiny structural bytecode or similarly flattened representation.

This VM is for SCXML control/executable structure, not a general-purpose expression language.

Things such as:

- assign
- if/elseif/else
- foreach
- send
- cancel
- raise
- invoke-related behavior
- log
- custom executable content where applicable

should execute efficiently while delegating expression/value semantics to the datamodel.

Flatten structured control flow wherever possible so that continuation state is primarily a program counter rather than a tree of runtime objects.

## Whole actor systems

The normal high-level embedding unit should be capable of being a complete `ta_system`, not merely one interpreter.

The system should internally own:

- SCXML instances/actors
- session registry
- actor lifecycle
- mailboxes
- internal/external SCXML event queues
- runnable scheduling
- timers and delayed sends
- `<cancel>`
- built-in SCXML I/O processor
- SCXML-to-SCXML sends
- SCXML invocation
- parent/invoked-session relationships
- `done.invoke.*`
- other required built-in SCXML behavior

An internal SCXML send such as a send to another SCXML session should not require a host-language roundtrip.

The conceptual boundary should be:

> Communication between SCXML sessions inside the packaged system is the library's responsibility. Communication crossing the system boundary is the host's responsibility.

Do not recursively execute another actor when sending to it. Enqueue the event and preserve actor isolation and SCXML run-to-completion semantics.

Investigate efficient system-wide event pools rather than heap-allocating an independent queue/event object graph for every actor.

## Built-in SCXML I/O processor

The built-in SCXML I/O processor must support efficient cross-instance sends.

SCXML targets such as internal, parent, invoked sessions and SCXML session targets should be represented semantically rather than repeatedly parsed as strings.

The core/system should understand SCXML addressing rules.

The host should not need to interpret strings such as `#_scxml_...`.

Use logical session IDs/handles internally and let the system runtime resolve local actors directly.

Only genuinely external/unresolved I/O should cross the host boundary.

## Fair cooperative scheduling

A broken/malicious actor must not monopolize the thread.

For example, an actor can contain an infinite eventless transition loop:

    A -> B -> A -> B -> ...

SCXML run-to-completion therefore cannot mean that the C scheduler blindly executes a macrostep until stability.

The execution kernel must be cooperatively resumable.

Use deterministic execution fuel/work budgets rather than wall-clock preemption.

Distinguish at least:

- actor quantum: fairness between actors
- system budget: fairness between the whole SCXML system and its embedding host

When an actor exhausts its quantum while still executing, it remains **runnable** and goes to the back of the runnable queue.

It must not require a new event to wake it.

Distinguish actor states conceptually such as:

- idle/stable
- runnable
- waiting for external work
- done

A system call such as:

    ta_system_run(system, budget)

must eventually return to the host even if one or more actors are permanently internally runnable.

## Continuation machine

Design execution as a continuation machine from the beginning.

Avoid:

- native fibers
- one native stack per actor
- stack copying
- recursive execution that cannot yield
- heavyweight heap-allocated continuations

Prefer a hybrid of:

1. an explicit SCXML semantic phase/program counter
2. compact cursors into precomputed tables
3. a tiny bytecode PC for executable content

For example, semantic phases might include:

- transition selection
- exit-set processing
- state exit
- onexit execution
- transition executable content
- entry-set processing
- state entry
- onentry execution
- internal-event processing
- eventless-transition processing
- stable

Do not create a program-counter state for every tiny operation. Process bounded chunks and retain cursors where loops may yield.

Flatten structured executable content at compile time so that nested `if`/`foreach` structures need minimal runtime continuation state.

The ideal suspended continuation should be very small—potentially only phase, flags, one or two cursors, and action bytecode PC/auxiliary state.

Stable actors should ideally require no continuation workspace.

Explore pooling continuation/workspace storage at the system level if that substantially reduces per-actor memory.

## `_event` semantics across suspension

Correct handling of SCXML `_event` is mandatory.

Scheduler yields are invisible to SCXML semantics.

If an actor starts processing an event, then enters eventless transitions and is suspended because its execution quantum expires, `_event` must retain the correct semantic event when the actor resumes.

Likewise, when an internal event becomes the event currently being processed, `_event` must correctly reflect that internal event.

Therefore the currently processed event must have a lifetime tied to SCXML processing semantics, not merely to dequeueing.

If events live in a shared event pool, a suspended actor may need to pin/reference its current event until it is no longer semantically current.

Correctly distinguish:

- scheduler yield
- reaching SCXML stability
- processing a new internal event
- processing a new external event
- initialization/eventless execution where there may be no current event

Do not allow scheduling implementation details to observably alter `_event`.

## Host/core communication

Optimize the ABI for every major FFI.

Prefer synchronous C commands from host to system:

    send
    send_many
    run
    resume
    advance_time
    inspect

and explicit tagged outcomes from system to host, such as:

- idle
- yielded
- external effect/request
- done
- error

Avoid making ordinary core→host communication depend on arbitrary callbacks.

For genuinely external asynchronous work, use explicit resumable request/response semantics:

    core returns external request
    host performs work
    host later resumes with request ID + result

Distinguish external operations that require a response from fire-and-forget emissions where useful.

However, recognize that datamodel operations are extremely hot and may justify a small synchronous vtable/callback interface rather than forcing one `run()/resume()` FFI roundtrip per expression evaluation.

Clearly separate:

- execution boundary communication
- synchronous semantic/datamodel services
- external asynchronous effects

Use integer handles/IDs across the ABI wherever possible.

Use borrowed views into host/core memory rather than serializing everything through JSON/Protobuf/etc.

Define precise ownership and lifetime rules for those views.

Support batching so high-level languages can submit many events and receive many outbound effects without excessive FFI crossings.

## Time

The core must not depend directly on an OS clock.

Time should be supplied explicitly by the host/system interface.

The system owns SCXML timer/delayed-send semantics, while the host supplies the notion of current time and integrates the next deadline with its event loop.

This should support:

- deterministic testing
- simulated time
- libuv
- asyncio
- JVM runtimes
- iOS/Android event loops
- database environments
- embedded runtimes

## Portability

The semantic core should strive for:

- portable C
- no mandatory libc where feasible
- no mandatory malloc
- no filesystem
- no threads
- no OS clock
- no blocking
- bounded native stack usage
- no pointer-rich persistent structures

It should be comfortable in WebAssembly, database extensions, mobile applications, servers and constrained environments.

## ABI design

Keep the public C ABI very small and opaque.

Language bindings should be thin enough to hand-write.

Bindings must not need to understand SCXML transition selection, queue semantics, session routing, invocation lifecycle or timer semantics.

Consider opaque system/chart types plus integer handles for actors, expressions, requests, event names, etc.

The ABI should remain stable even if internal representations change substantially.

## Performance target

The normal transition path should approach:

    event ID
      -> indexed candidate transitions
      -> condition evaluation
      -> bitset/table-based conflict/exit/entry operations
      -> compact executable-content VM
      -> updated configuration

Avoid on hot paths:

- XML traversal
- state-name hash lookups
- repeated event-name parsing
- heap allocation
- recursive hierarchy traversal
- unnecessary host-language value conversion
- FFI roundtrips for internal actor communication

Use compile-time work aggressively to reduce runtime work.

## Your task

Develop this architecture rigorously rather than merely agreeing with these ideas.

Identify where the requirements conflict.

For each major subsystem, describe:

1. responsibilities
2. data representation
3. ownership/lifetimes
4. hot execution path
5. suspension/resumption behavior
6. memory costs
7. host ABI implications
8. important SCXML specification corner cases

In particular, derive:

- packed Chart IR
- compiled System IR
- mutable instance layout
- system/actor layout
- event representation and event pool
- `_event` lifetime model
- event-name representation and matching
- mailbox design
- runnable scheduler
- timer/delayed-send structure
- continuation representation
- executable-content bytecode
- datamodel interface
- built-in SCXML I/O processor
- invocation machinery
- external effect protocol
- host/core ABI
- inspection/reconstruction interface
- memory allocation strategy

Explicitly walk through these scenarios:

1. normal external event causing a transition
2. eventless transition chain reaching stability
3. infinitely looping eventless actor being fairly rescheduled
4. several infinitely runnable actors sharing the thread
5. actor A sending to actor B internally
6. invoked SCXML child sending/replying to its parent
7. delayed cross-actor send
8. external asynchronous I/O that suspends an actor
9. scheduler suspension during executable content
10. scheduler suspension during eventless processing while `_event` remains observable
11. an internal event replacing the previously current `_event`
12. 100,000 mostly-idle actors sharing a small number of active continuation workspaces

Challenge unnecessary abstractions. Prefer simple packed arrays, integer IDs, bitsets, arenas/pools and explicit state machines over general-purpose infrastructure.

The desired end state is not merely "an SCXML parser/interpreter."

It should be possible to compile an entire actor system into a compact immutable image, load it into almost any host, provide bounded mutable memory and a datamodel/external-effect implementation, and run it as a **tiny portable cooperative actor-system VM with full SCXML semantics**.

## Tinyactors SCXML VM: architecture

I've kept the `ta_` prefix and treated this as the Tinyactors runtime. The design is organised around a few results that I think carry most of the weight. I state each one first and derive it in the sections below.

**Six results that shape everything else:**

1. **Number states in document preorder.** Then every subtree is a contiguous ID range, exit order is "descending IDs", entry order is "ascending IDs", a transition's exit scope is a range check, and two transitions conflict exactly when their ranges overlap. Most of the SCXML algorithm becomes bitset-range operations with no recursion.
2. **The internal event queue is always empty when an actor is stable.** So the internal queue, the enabled/exit/entry sets and all cursors can live in a pooled workspace. A stable actor holds none of them.
3. **`_event` only has to be pinned until the actor is stable.** SCXML keeps `_event` bound to the last processed event. But between stability and the next dequeue, no SCXML code can run in that actor. The core can therefore release its pool slot at stability, and the datamodel keeps its own binding. Event-pool pressure then scales with the number of *unstable* actors, not the number of actors.
4. **An event class is one integer per chart.** It is the deepest node reached in a trie of the chart's event-descriptor tokens. A transition matches if its descriptor node is an ancestor-or-self of the class node, which is a pre/post interval check.
5. **Three host channels, not one.** Execution-boundary commands and outcomes have no callbacks. The datamodel is a synchronous vtable, because it's too hot for round trips. External effects go out through a batched outbox, with request/response for the few that need an answer.
6. **Standard SCXML never needs to suspend a macrostep for I/O.** Suspension exists for fairness (fuel) and for one opt-in extension: async custom actions. Everything else is ordinary event passing.

---

## 1. Where the requirements conflict

| Conflict | Resolution |
|---|---|
| **Run-to-completion vs fairness.** An eventless loop never finishes its macrostep. | Make the macrostep resumable and yield on fuel. Yields are invisible *because actors are isolated*: no other SCXML session can see a half-finished macrostep. If two actors' datamodels share a mutable host object, interleaving becomes visible through that object. Treat such objects as external state, with the same caveat as I/O. |
| **Pooled workspaces vs infinite loops.** W looping actors can hold all W workspaces forever, so stable actors with mail never get one. | No fixed memory budget fixes this, because the state of a non-terminating macrostep can't be dropped. Options: (a) let the workspace pool grow through the allocator hook, (b) spill workspaces to a cold pool, (c) set a deterministic `max_macrostep_fuel` watchdog that faults the actor and reports it to the host. Default is (c) on, plus an optional growth hook. The guarantee is fairness among actors that hold workspaces, and no starvation unless W actors are all non-terminating at once. |
| **No callbacks vs hot expression evaluation.** | Datamodel calls are a synchronous vtable, and the only callbacks in the system. They may not re-enter mutating system APIs; the core checks a flag and returns `TA_EREENTRANT`. |
| **Datamodel agnosticism vs cross-actor payloads.** Sends carry values, and actors must stay isolated. | Payloads are opaque datamodel handles built by a *capture* operation that must produce an immutable or deep-copied value. Same-kind datamodels pass the handle directly. Cross-kind or external traffic goes through optional `export`/`import`. The core never sees values. |
| **Fixed memory vs unbounded SCXML** (queue lengths, `foreach`, raises in loops, dynamic invokes). | Everything is bounded, with *defined* overflow behaviour: a host send gets `TA_EFULL` back-pressure; an SCXML send gets `error.communication` raised in the sender; overflowing an actor's own internal queue faults the actor; a failed spawn on invoke gets `error.execution`. Growth is possible through the allocator hook. |
| **Core fuel vs expression cost.** The core can't preempt `while(true){}` inside JavaScript. | The core bounds *structural* work. It passes a fuel hint to the datamodel, which may charge cost or fail with `error.execution`. True expression preemption is the datamodel's job (QuickJS interrupt handler, Python tracing, etc.). |
| **Ahead-of-time compilation vs dynamic SCXML** (`<invoke srcexpr>`, `<content expr>` that yields a document). | `srcexpr` resolves against the image's chart-name table. A runtime-generated document needs `ta_compile` linked in; otherwise it raises `error.execution`. |
| **Position independence vs host pointers.** | Host pointers (datamodel contexts, payloads, iterators) live only in mutable side tables, never in images, and never inside links. A snapshot needs the datamodel to export them. If the datamodel stores its values in a ta-provided arena (as the built-in `ta_expr` VM does), the whole system is POD: it can be memcpy'd, live in Postgres DSM, or be snapshotted with zero cooperation. |
| **Async I/O "suspending" an actor vs SCXML semantics.** | SCXML's own pattern is send, become stable, receive a reply event. True mid-macrostep suspension is only for `<ta:await>`-style custom actions. It's legal (external events just queue), but it holds a workspace. |
| **Small standalone kernel vs no queues in bindings.** | Keep `ta_core` as an internal and expert layer. The recommended embedding unit is a `ta_system`, even for one machine; `max_actors=1` costs a few hundred bytes. |
| **Spec ambiguity:** `<if>` cond error. §5.9 says treat it as false and raise `error.execution`; the block-abort rule says stop the block. | Make it a compile-time policy flag that defaults to "false + raise, continue the block". Verify against the W3C IRP tests. |

---

## 2. Layering (revised)

```
ta_compile   offline/optional: XML → validated, analysed Chart IR → System Image
ta_core      kernel: microstep algorithm, continuation machine, bytecode VM
             (no allocation, no event pool; talks to an internal "event store" interface)
ta_system    actors, sessions, event pool, mailboxes, scheduler, timers,
             SCXML I/O processor, invoke lifecycle, outbox, inspection
ta_dm_*      datamodel adapters: null (core-native), ta_expr (tiny VM), quickjs, …
ta_runtime   optional: OS clock, libuv/asyncio glue, file loading, logging, Basic HTTP
```

**What changed from the proposal:**

- `ta_core` and `ta_system` ship as one library with a strict internal boundary: a static function table, not an ABI. Every binding uses the `ta_system` API.
- Datamodels are their own layer. They're the one piece hosts commonly write.
- `ta_compile` is not on the runtime's dependency path.

---

## 3. Packed Chart IR

**Responsibilities.** Everything static: hierarchy, transitions, precomputed domains and entry closures, descriptor trie, bytecode, send/invoke specs, the expression table (for datamodel preparation), and debug strings that can be stripped.

**Representation.** One little-endian blob with a header and a section table. Everything inside is offsets and u16 indices; a u32 build option covers more than 65,535 states. Big-endian loaders byte-swap once or refuse.

```c
struct ta_chart_hdr {
  u32 magic, size, crc; u16 ver, flags;
  u16 n_states, n_trans, n_blocks, n_invokes, n_data, n_hist, n_trie, max_foreach;
  u16 inst_size, ws_size;      // derived by compiler
  u8  dm_kind, binding;        // early/late
  u32 sec_off[TA_NSEC], sec_len[TA_NSEC];
};
```

**State table** (structure of arrays, about 24–28 B per state in ROM). States are in preorder, with ID 0 as the `<scxml>` root.

| field | purpose |
|---|---|
| `parent[]`, `depth[]` | hierarchy without recursion |
| `subtree_end[]` | descendants of s = `(s, subtree_end[s]]` |
| `kind[]` | atomic / compound / parallel / final / hist-shallow / hist-deep |
| `tr_lo, tr_ev_hi, tr_hi` | `[lo,ev_hi)` eventful and `[ev_hi,hi)` eventless transitions, each in document order |
| `onentry_lo/hi`, `onexit_lo/hi` | block ID ranges (one block per `<onentry>` element, for error isolation) |
| `invoke_lo/hi`, `data_lo/hi` | invoke-spec and data-element ranges |
| `init_entry` | static entry list for default initial |
| `hist_slot` | offset into the instance's history storage |
| `donedata` | payload-spec ID |
| `final_children` | list (compound) used by "is in final state" |

Global masks: `atomic_mask`, `final_mask`, `has_history_child_mask`.

**Transition table** (about 20 B each): `source`, `domain` (the static LCCA, or `DYNAMIC` when there is a history target), `flags` (targetless/internal/history-target), `cond` (an expression ID *or* a core-native predicate such as `IN(state)` or `TRUE`), `block`, `desc_lo/hi` (trie nodes), `entry_lo/hi`, `targets_lo/hi`.

**Precomputation, and why each item holds:**

- **Exit scope.** `exitset(t) = config ∩ (domain, subtree_end[domain]]`. It's one masked bitset range.
- **Conflict.** Preorder ranges are either nested or disjoint. A transition with targets always has a non-empty exit set, because its source, or an active descendant of an internal source, lies in the range. Targetless transitions never conflict. So two transitions conflict iff their domain ranges overlap: O(1).
- **Static entry closure.** For a transition with no history targets, the compiler emits the sorted list of states to enter: targets, ancestors up to the domain, other regions of crossed parallels, and recursive default initials. Each entry is tagged with whether it's a default entry, which decides whether to run its `<initial>` content. Non-conflicting transitions have disjoint domain ranges, so their closures are disjoint, and the runtime entry set is a disjoint union of static lists plus dynamic history expansion.
- **History-target transitions.** Their domain depends on the *stored* history (effective targets), so it's computed at runtime with bounded parent walks. This is a real corner case; most implementations get it wrong.
- **Active child of compound s.** It's the first set config bit in `(s, subtree_end[s]]`, because the child precedes its descendants in preorder. That's a word scan, which makes shallow-history recording cheap.

**Ownership.** Owned by the host. The image is immutable and must outlive every system using it. Suitable for ROM, flash or mmap, and one image is shared by any number of systems and instances.

---

## 4. Event names and matching

**Representation of an event name inside the system** (a u32 plus a tag):

| tag | meaning | materialised string |
|---|---|---|
| `STATIC` | global atom from the image (all literal event names, `done.state.<id>`, literal `done.invoke.<id>`, `error.*`) | atom table |
| `DONE_INVOKE` | invoke-spec index, for generated invoke IDs | rendered lazily |
| `DYNAMIC` | bytes in the payload arena (from `eventexpr` or the host) | the bytes themselves |

Dynamic names are **not interned**. Interning attacker-chosen names grows a table without bound. Matching doesn't need atoms; it needs a class.

**Descriptor trie (per chart).** The compiler tokenises every descriptor on `.`. The `foo.*` form is normalised to `foo`, and `*` is the root terminal. Edges are labelled with global token atoms. Nodes are numbered in preorder, with `post[]` stored.

**Classification** happens once per event, at dequeue, for the receiving chart. Walk the event's tokens down the trie as far as edges exist; the class is the deepest node reached. For dynamic bytes, each token is looked up in the image's perfect hash of tokens. An unknown token stops the walk, which is correct because nothing below it can match.

**Match test:** transition t matches iff some descriptor node d in t satisfies `pre[d] ≤ cls ≤ post[d]`. That gives prefix matching on whole tokens (`foo` matches `foo.bar` but not `foobar`), and `*` matches everything, all with integer compares.

**Static atoms** use a sparse sorted `(atom → class)` array per chart, containing only atoms that reach a non-root node; lookup is a binary search. Generated invoke IDs (`stateid.platformid`) get a precomputed class from the `done.invoke.stateid` prefix, so descriptors like `done.invoke.s1` work without string building.

**Hot path cost:** for static names, about a log₂(k)-step search. For dynamic names, one hash per token. Per candidate transition, two compares.

---

## 5. System image (System IR)

**Contents:**

- a chart directory (offsets to embedded chart blobs, plus the per-chart datamodel kind)
- the global atom table and token perfect hash
- a chart-name table, for `<invoke src>`, `srcexpr` and spawn by name
- actor templates for boot: an initial actor set with well-known names, which the host addresses by atom
- system limits suggested by the compiler (pool sizes, quotas)
- size classes: the distinct `inst_size` and `ws_size` values of all charts

**Static invoke resolution:**

- `<invoke type="scxml" src="child.scxml">` compiles to a **chart index**.
- Inline `<content><scxml>…` is compiled as an anonymous chart in the image.
- `srcexpr` evaluates to a string that is looked up in the name table at runtime.

A single blob means one mmap gives you the entire actor system.

---

## 6. Mutable instance / actor layout

**Three kinds of state, kept separate:**

| kind | lifetime | where |
|---|---|---|
| immutable | image | chart blob |
| persistent actor state | session lifetime | actor slab (size class from chart) |
| execution workspace | one macrostep (including yields) | pooled workspace |

**Actor record.** Fixed-stride header plus a chart-dependent tail; `ta_instance_size(chart) = 32 + tail`.

```c
struct ta_actor_hdr {          // 32 B
  u16 chart; u8 status; u8 flags;     // IDLE/READY/WAIT_WS/WAIT_EXT/DONE, fault, cancel_pending
  u32 gen;                            // session generation
  u32 mbox_head, mbox_tail;           // intrusive event-pool links
  u16 mbox_count; u16 ws;             // workspace index or NONE
  u32 sched_next;                     // run-queue / ws-wait / teardown worklist link
  u32 parent;  u16 parent_inv;        // parent slot + invoke index in parent
  u16 _pad;
  u32 sendseq;                        // generated sendids
};
// tail (chart-dependent, offsets from header):
//   uintptr dm_ctx
//   u64     child[n_invokes]         // session handles (index|gen)
//   bits    config[n_states]
//   bits    entered_once[n_data_states]   // late binding only
//   history storage: deep → bits over subtree range; shallow → u16 child id
```

**Size example:** 40 states, 1 invoke, 1 deep history over 10 states. Header 32 B + ctx 8 + child 8 + config 8 + history 2, so **about 58 B, padded to 64 B**. A null-datamodel, 8-state chart with no invokes comes to about 40 B.

**Session identity.** A handle is `index | gen << 21`. It stays below 2⁵³ so JavaScript and Java can carry it as a double or long. `_sessionid` is the canonical string `ta<sys>.<idx>.<gen>`, rendered on demand and never stored. `_name` is a static atom.

---

## 7. Continuation representation and workspace

**Workspace** (pooled; size from `ws_size` of the chart's class):

```c
struct ta_ws {
  u8  phase, flags; u16 cur;          // state or transition cursor
  u16 sub;  u16 blk;                  // block index within state; current block id
  u32 pc;                             // bytecode PC
  u32 cur_event;                      // pinned event slot (or NONE)
  u32 iq_head, iq_tail;               // internal queue (event-pool links)
  u32 microsteps;  u32 macro_fuel;    // watchdog
  uintptr iter[max_foreach];          // dm iterator handles
  u32 iter_idx[max_foreach];
  bits enabled[n_trans], exitset[n_states], entryset[n_states],
       default_entry[n_states], to_invoke[n_states];
  u8  scratch[64];                    // name/origin rendering, error message
};
```

Size: at 40 states and 40 transitions, about **48 + 5×8 + 64 ≈ 160 B**. At 1,000 states, about 700 B, which is why pooling matters for big charts.

**Phases.** Each one sits where a loop can yield, not one per operation.

```
INIT_DATA (early binding, cursor=data idx) → INIT_ENTER
LOOP_EVENTLESS: SELECT(cls=NULL) → if none → DEQ_INTERNAL
DEQ_INTERNAL:   if iq empty → INVOKE_START else bind, SELECT(cls)
SELECT          cursor=atomic state; memo per ancestor; partial enabled bits
EXIT_RECORD     record history for exitset
EXIT            cursor=state (descending), sub/pc = onexit blocks; cancel invokes
TRANS_CONTENT   cursor=transition (doc order), pc
ENTER           cursor=state (ascending), sub/pc = data-init, onentry, initial content,
                history default content, done.state generation
INVOKE_START    cursor over to_invoke ∩ config
STABLE_CHECK    → release cur_event, free ws, actor→IDLE (or READY if mail)
PRE_EXTERNAL    finalize (invoke match), autoforward, then SELECT(cls)
EXIT_INTERP     top-level final: exit all, done.invoke to parent, DONE
WAIT_EXT        custom async action pending (request id in ws)
```

**Suspended continuation** = `phase, cur, sub, blk, pc` (12 B) plus the bitsets that are already live. A yield costs nothing extra; it simply returns. A stable actor has **no** workspace.

**Admission.** An actor with mail but no workspace goes to a FIFO `ws_wait` queue. When a workspace frees, the head of that queue moves to the run queue. An actor that is mid-macrostep never gives up its workspace.

---

## 8. Executable-content bytecode

**Encoding.** A u8 opcode with aligned u16 operands, which decodes faster than varints and is still compact. The block table maps each block to `(pc_lo, pc_hi, foreach_base)`.

```
ASSIGN loc, expr            ASSIGN_CONTENT loc, content
JF expr, off                JMP off            // flattened if/elseif/else
FE_BEGIN slot, fe_id, off   FE_NEXT slot, off  FE_END slot
RAISE name                  SEND spec          CANCEL key | CANCEL_EXPR expr
LOG spec                    SCRIPT id          CUSTOM id    CUSTOM_AWAIT id
END
```

**Flattening.** `if` becomes jumps and needs no runtime state. `foreach` nesting depth is static, so each level gets a fixed iterator slot. Continuation state is therefore just `pc` plus the live iterator slots.

**Fuel.** Before each op the VM charges 1 plus whatever cost the datamodel reports. At zero it saves `pc` and returns `YIELD`.

**Errors.** The datamodel returns `TA_DM_EEXEC`. The core enqueues `error.execution` on the internal queue, calls `iter_end` on slots at or above `foreach_base`, and jumps to `pc_hi`, skipping the rest of the block. Other blocks still run.

**`foreach` semantics.** `iter_begin` takes the shallow copy the spec requires. An invalid item location is an error, and the loop body does not run.

---

## 9. Events, event pool, and `_event` lifetime

**Event slot** (32 B, structure of arrays plus a side table for host pointers):

```c
struct ta_ev { u32 next; u32 name; u32 sendid; u32 origin;   // session handle or ext-origin ref
               u16 inv; u8 type; u8 flags; u16 refs; u16 arena_len; u32 arena_off; };
uintptr ev_payload[N];   // dm payload handle (side table)
```

**Pool.** One system-wide free list of slots plus a payload arena for dynamic names and origin strings. Queues link slots intrusively through `next`, so mailboxes cost 8–10 B per actor and nothing extra per message.

**Fan-out** only happens with autoforward. It copies the 32 B header and shares the payload through `dm.retain`.

**`_event` lifetime model:**

| moment | action |
|---|---|
| dequeue (internal or external) | release previous `cur_event` (`refs--`, free → `dm.release(payload)`); `cur_event = e`; `dm.bind_event(ctx, env)` |
| scheduler yield | nothing; `cur_event` stays pinned, datamodel binding unchanged |
| eventless microsteps | nothing; `_event` stays the last processed event, as the spec requires |
| `INVOKE_START` | still bound (invoke params may read `_event`) |
| stability | core releases the slot; the datamodel keeps its own binding |
| initialisation before any event | `cur_event = NONE`; the datamodel represents `_event` as unbound |

Event fields are exposed lazily through `env` accessors that stay valid until stability. A datamodel that exposes `_event` *outside* SCXML execution (to host code) must materialise it at bind time.

**System variables.** `_sessionid`, `_name` and `_ioprocessors` are supplied once through `ctx_create`. The datamodel must reject assignments to them (and to `_event`) with `error.execution`. Static `<assign location="_event…">` is also rejected by the compiler.

---

## 10. Mailboxes, scheduler and fuel

**Mailbox.** An intrusive FIFO in the event pool with a per-actor quota (`mbox_count`). If an actor is mid-macrostep or in `WAIT_EXT`, new events just append.

**Actor status transitions:**

```
IDLE --event/timer--> READY (push run queue)
READY --pop, no ws--> WAIT_WS (push ws_wait)
READY --pop--> run quantum --> {still unstable → READY (tail)} |
                               {stable, mail → READY (tail)} | {stable → IDLE} |
                               {custom await → WAIT_EXT} | {top final → DONE}
WAIT_EXT --ta_complete--> READY
```

**Run loop:**

```c
ta_run(sys, now, budget, out):
  fire_due_timers(now)                     // charged to budget
  while budget > 0 && runq nonempty && outbox has room:
    a = pop(runq); q = min(actor_quantum, budget)
    used = step(a, q)                      // core continuation machine
    budget -= used; requeue per status
  return {IDLE | YIELDED | OUTBOX_FULL | WAITING_ONLY, next_deadline}
```

**Fuel units:** 1 per bytecode op, 1 per state exited or entered, 1 per transition considered, and the datamodel-reported cost for each datamodel call. Defaults are a quantum of 256 and `max_macrostep_fuel` of 10⁶ (the watchdog).

**Guarantees:**

- `ta_run` returns after at most `budget + one op + one datamodel call`.
- An actor that exhausts its quantum while unstable is requeued **without** needing a new event.
- Ordering is deterministic: FIFO run queue, FIFO mailboxes, timers ordered by `(deadline, seq)`, and logical time fixed for each `ta_run` call.

---

## 11. Timers and delayed sends

**Structure.** A binary min-heap of u32 indices into a timer slab. Each 32 B entry holds: `deadline u64, seq u32, owner handle, sendid key, event slot, target handle/kind, heap_pos, owner_next/prev`. The per-owner intrusive list makes `<cancel>` and session teardown O(timers of that owner).

**At send time.** All attributes and the payload are evaluated, and the event slot is built *now*. The spec requires send-time evaluation. The target is resolved now as well.

**At fire time:**

- Local target alive: enqueue in its mailbox.
- Local target dead: enqueue `error.communication` to the sender, if the sender is alive.
- External target: emit a `SEND` effect.

**Sendid keys:**

- Literal `id` becomes `STATIC` atom.
- Generated IDs use the canonical form `ta:<seq>`, assigned through `dm.set_str(idlocation)`.
- `<cancel sendidexpr>` parses the canonical form or looks up the atom. A string that's neither cancels nothing, and nothing is allocated.
- Cancel only searches the canceller's own list.

**Time.** A u64 in host ticks; the image stores delays in ms and the system config sets `ticks_per_ms`. The system never reads a clock. `ta_next_deadline()` drives libuv, asyncio, Android `Handler`, Postgres background workers, or simulated time.

---

## 12. Built-in SCXML I/O processor and addressing

**Compiled target kinds:** `INTERNAL`, `PARENT`, `INVOKE(idx)` (literal invoke ID in the same chart), `SELF_EXT`, `SESSION(str)`, `EXTERNAL(uri)`, `EXPR(expr)`.

**Runtime resolution** (only for `targetexpr` and session strings), as one parse:

- `#_internal` goes to the internal queue.
- `#_parent` goes to the parent handle.
- `#_scxml_` followed by our canonical form becomes a (idx, gen) handle, checked for liveness.
- `#_<id>` matches the actor's active invoke IDs: literal IDs through the static table, generated IDs by parse.
- Anything else, including foreign session IDs and URIs, becomes an external `SEND` effect.

**Errors:**

- An unsupported `type`, or a malformed target, raises `error.execution`.
- An unreachable target (dead session, inactive invoke, full mailbox, or a host-reported failure) raises `error.communication`.

**Field values:**

- Events between SCXML sessions get `origintype` = the SCXML processor URI, and `origin` = `#_scxml_<sessionid>` rendered lazily.
- `type` is `external`.
- `raise` and `#_internal` produce `internal`; errors produce `platform`.

**Ownership of the payload.** The sender's datamodel captures it, the event slot owns it, and it is released when the event slot frees.

---

## 13. Invocation

**Per-instance storage:** `child[n_invokes]`. Each invoke element can only be active once, because its state is only active once. That makes storage static: no tables, no allocation.

**Start** (phase `INVOKE_START`, for states in `to_invoke ∩ config`, in document order):

1. Generate or evaluate the invoke ID. Write `idlocation` if present.
2. `dm.payload(params)`.
3. For SCXML type: allocate an actor slot, set `parent` and `parent_inv`, call `ctx_create` with params (which override matching top-level data), and push the child to the run queue to initialise.
4. For any other type: emit an `INVOKE_START` request effect.

**Events from the child** carry `inv = parent_inv`. At parent dequeue, if `child[inv]` doesn't match the origin handle, the event is dropped (stale or cancelled). Otherwise `<finalize>` runs, then autoforward, then transition selection.

**Child completion.** Exit-interpreter runs, then `done.invoke.<id>` (name tag `DONE_INVOKE`, with donedata payload) goes to the parent's mailbox. It is FIFO after everything the child sent before.

**Cancel on state exit** (in `EXIT`):

- `child[i]` is set to NONE and the child is marked cancel-pending.
- Teardown runs from a worklist through `sched_next`, so there's no recursion even for deep invoke trees: remove from queues, drain the mailbox, cancel timers, cancel its children, `ctx_destroy`, then `gen++`.
- No `done.invoke` is sent.
- Whether a cancelled child runs its `onexit` handlers is a policy flag; the default is no.
- External invokes get an `INVOKE_CANCEL` effect.

**Corner cases:**

- An invoke in a state that is entered and exited within one macrostep never starts.
- An invoke in a state that is re-entered gets a fresh child.
- `<finalize>` executes with the child's event bound as `_event`.

---

## 14. Datamodel interface (minimum for full SCXML)

```c
typedef uintptr_t ta_dmh;      // ctx, payload, iterator: datamodel-defined
typedef struct ta_dm_ops {
  u32 abi, flags;
  // chart-level (shared): compile expression table (ids, kinds, source) into dm form
  int  (*prepare)(void *dm, const ta_chart *c, ta_dmh *prog);
  void (*unprepare)(void *dm, ta_dmh prog);
  // instance-level
  int  (*ctx_create)(void *dm, ta_dmh prog, const ta_sysvars *sv, ta_dmh params, ta_dmh *ctx);
  void (*ctx_destroy)(void *dm, ta_dmh ctx);
  int  (*init_data)(ta_env *e, u16 data_id);            // expr/src/inline; params override
  int  (*cond)(ta_env *e, u16 expr);                    // 1/0 or <0
  int  (*assign)(ta_env *e, u16 loc, u16 expr_or_content);
  int  (*eval_str)(ta_env *e, u16 expr, ta_str *out);   // valid until next dm call
  int  (*set_str)(ta_env *e, u16 loc, ta_str v);        // idlocation
  int  (*script)(ta_env *e, u16 id);
  int  (*payload)(ta_env *e, u16 spec, ta_dmh *out);    // namelist+params+content → immutable
  ta_dmh (*retain)(void *dm, ta_dmh p);  void (*release)(void *dm, ta_dmh p);
  int  (*bind_event)(ta_env *e);                        // fields via ta_env_event(e, FIELD,…)
  int  (*iter_begin)(ta_env *e, u16 fe, ta_dmh *it);
  int  (*iter_next)(ta_env *e, u16 fe, ta_dmh it, u32 idx);  // assigns item/index; 1/0/<0
  void (*iter_end)(void *dm, ta_dmh it);
  int  (*custom)(ta_env *e, u16 id, ta_custom_out *o);  // optional
  int  (*export)(void*, ta_dmh, ta_buf*); int (*import)(void*, ta_bytes, ta_dmh*); // optional
} ta_dm_ops;
```

**What the core needs back:** only bool, borrowed string, and opaque handle. There is no value system.

**Environment services:** `ta_env` gives read-only core services: `ta_in(e, state)`, event field access, fuel charging, and error-message setting.

**`In()` handling.** The compiler lowers a literal `In('s')` cond to a native predicate, so the null datamodel has **zero callbacks**. `In()` reads the live config, so during transition content it correctly reflects "exited but not yet entered".

**Bindings:**

- Python: payload = `PyObject*` (captured as an immutable copy), ctx = a dict.
- Ruby: `VALUE` with GC registration.
- Postgres: `Datum` in a memory context.
- Embedded: `ta_expr` arena offsets.

---

## 15. Host/core ABI and external effect protocol

```c
size_t  ta_system_size(const ta_image*, const ta_sys_config*);
int     ta_system_init(void *mem, size_t n, const ta_image*, const ta_sys_config*,
                       const ta_dm_binding *dms, size_t ndm);   // dm vtables per kind
int     ta_spawn(ta_system*, u32 chart_or_name_atom, ta_dmh params, u64 *actor);
int     ta_send(ta_system*, const ta_event_in*);                // EFULL → host keeps payload
size_t  ta_send_many(ta_system*, const ta_event_in*, size_t n); // returns accepted prefix
ta_run_result ta_run(ta_system*, u64 now, u32 budget);
u64     ta_next_deadline(const ta_system*);
const ta_effect *ta_outbox(ta_system*, size_t *n);              // contiguous view
void    ta_outbox_consume(ta_system*, size_t n);
int     ta_complete(ta_system*, u32 req_id, int status, ta_dmh result);
int     ta_report_send_error(ta_system*, u64 actor, ta_str sendid);
int     ta_inspect(ta_system*, u64 actor, ta_actor_info*);
u32     ta_atom(const ta_image*, ta_str);                       // name → atom (0 if absent)
```

**`ta_event_in`:** `target, name (atom|str), payload, sendid, origin, origintype, invokeid (for external invokes), flags`. If the host sets `origin`, the actor can reply to it; a non-local target is emitted back as an external `SEND` with the same string. Routing stays entirely the host's business.

**Effect kinds** (one tagged struct; views into system memory):

| kind | response? |
|---|---|
| `SEND` (target URI, type, name, sendid, payload) | no; optional `ta_report_send_error` |
| `INVOKE_START` (type, src, invokeid, params, req_id) | events arrive via `ta_send(invokeid=…)`; done through `ta_complete` |
| `INVOKE_CANCEL` | no |
| `CUSTOM` / `CUSTOM_AWAIT` (req_id) | await: yes, through `ta_complete`, and the actor is in `WAIT_EXT` |
| `LOG` (label, string) | no |
| `ACTOR_DONE` (actor, donedata) | no; slot reclaimed after consume |
| `FAULT` (actor, code: watchdog / queue overflow / dm fatal) | no |

**Ownership and lifetime rules:**

- Image: owned by the host, outlives the system.
- System memory: one host block; never freed internally; growth only through the optional `ta_sys_config.alloc` hook, which adds segments.
- Views returned by the system (`ta_str`, effects): valid until the next mutating call (`ta_run`, `ta_send*`, `ta_outbox_consume`, `ta_complete`).
- Payload in an effect: borrowed until `consume`. The host retains it if it needs it longer.
- Payload passed in: ownership transfers on success and stays with the caller on error.
- Datamodel callbacks: may call `ta_env_*` only. Mutating APIs return `TA_EREENTRANT`.
- Threads: one system is single-threaded. Separate systems may run on separate threads. Cross-system traffic is external (a lock-free injection inbox can live in `ta_runtime`).

**Batching.** A typical high-level loop costs one `send_many`, one `run`, one outbox read and one consume per tick, regardless of how many actors talk to each other internally.

---

## 16. Inspection and reconstruction

- **`ta_inspect`:** status, chart, config as a bitset view, mailbox depth, phase, current event name, outstanding timers. State names are available through `ta_chart_state_name` when debug strings are present.
- **Snapshot:** the arenas are POD (actor slab, event pool, timers, workspaces, dynamic arena), plus `export` for payloads, iterators and contexts. Snapshots work mid-macrostep too, since a continuation is just integers.
- **Restore or migrate one actor:** `ta_actor_restore(config list, history, dm ctx)` validates legality: ancestors closed, exactly one child per active compound, all children per active parallel.
- **Deterministic replay:** record `send`, `now`, `complete` and `report_send_error`. With a deterministic datamodel, outputs replay bit-for-bit.

---

## 17. Memory allocation strategy

One host block, carved at init into:

1. actor slabs, one per size class from the image
2. workspace pools, one per workspace size class
3. event pool and payload side table
4. payload arena (dynamic names and origins), as a bump-with-free-list ring
5. timer slab and heap
6. run-queue state (intrusive, so free)
7. outbox ring

Size-class free lists are O(1) and deterministic. The core does **zero** allocation; the system allocates only from these pools.

**Budget for scenario 12:** 100k actors at 64 B, 64 workspaces at 160 B, a 4,096-slot event pool, and 1,024 timers.

| item | bytes |
|---|---|
| actors | 6.4 MB |
| workspaces | 10 KB |
| event pool + payload table | 160 KB |
| timers | 32 KB |
| **total** | **≈ 6.6 MB** |

In a high-level host, **datamodel contexts dominate**: 100k Python dicts are roughly 100 MB. Mitigations are lazy `ctx_create` (only on first `init_data` or evaluation for charts whose data is all late-bound), the null datamodel, or the `ta_expr` arena datamodel.

---

## 18. Scenario walkthroughs

**1. External event causes a transition.**
1. `ta_send` allocates a slot and appends it to B's mailbox. B was IDLE, so it becomes READY and joins the run queue.
2. `ta_run` pops B, acquires a workspace, and dequeues the event (`PRE_EXTERNAL`).
3. Classification is a binary search giving `cls`. Finalize doesn't apply.
4. `SELECT`: one atomic state; its ancestor walk finds t with an interval match and `cond` (one datamodel call).
5. Exit set = config ∩ range. `EXIT` runs descending (onexit blocks through the VM), then `TRANS_CONTENT`, then `ENTER` from t's static list ascending (onentry).
6. Eventless check: none. Internal queue empty. Nothing to invoke. Stable.
7. Release the pinned event, free the workspace, B goes IDLE.

There are no allocations, hashes or strings in this path.

**2. Eventless chain reaching stability.** After step 5 above, `LOOP_EVENTLESS` selects from the eventless ranges for each atomic state. Microsteps repeat until none are enabled, then `DEQ_INTERNAL` finds the queue empty and the actor is stable. `_event` is the original external event throughout.

**3. Infinite `A→B→A`.** Each microstep costs about 4–6 fuel. After a quantum of 256, the continuation is `(LOOP_EVENTLESS)`; the actor goes to the tail of the run queue as READY and keeps its workspace and pinned event. No new event is needed for it to run again. If `max_macrostep_fuel` is set, a `FAULT` effect eventually fires and the actor is either stopped or left parked, by policy.

**4. Several infinite actors.** They round-robin by quantum. `ta_run(budget)` returns `YIELDED` once the budget is spent, and the host reschedules soon (`setImmediate`, `call_soon`). Other actors with mail are interleaved fairly as long as workspaces are available. Otherwise they wait in `ws_wait`; this is the starvation case from §1, bounded by the watchdog.

**5. A sends to B internally.** A's VM executes `SEND spec`:
1. Target kind `SESSION`, resolved at send time to a handle with a generation check.
2. `dm.payload` captures the payload.
3. A slot is allocated and appended to B's mailbox; B becomes READY.
4. A continues its macrostep. B never runs recursively.

No FFI and no strings are involved; `origin` is a handle that's rendered only if B reads `_event.origin`.

**6. Child replies to parent.** Child `SEND target=#_parent` enqueues to `parent` with `inv=parent_inv`. At parent dequeue: the generation check against `child[inv]` passes, `<finalize>` runs with `_event` bound, then selection. On child completion, `done.invoke.<id>` with donedata follows in FIFO order.

**7. Delayed cross-actor send.**
1. At send time: evaluate, build the slot, resolve the target handle, insert a heap entry at `now + delay` with `seq`, and link it on A's timer list. A's `idlocation` gets `ta:<seq>`.
2. Later `ta_run(now')` fires due timers in `(deadline, seq)` order: B alive → mailbox; B dead → `error.communication` to A.
3. A `<cancel>` before that point unlinks the entry and frees the slot.

**8. External async I/O.**
- *Standard pattern:* A sends to `https://…` (type is host-supported), producing a `SEND` effect. A becomes stable and IDLE. The host later calls `ta_send(A, reply)`. A holds no workspace while waiting.
- *Extension:* `<ta:await>` emits `CUSTOM_AWAIT{req}`. A enters `WAIT_EXT` holding its workspace and pin, and incoming events queue. `ta_complete(req, result)` makes it READY; the result is assigned through the custom out-location and execution resumes at the next `pc`.

**9. Suspension inside executable content.** A `foreach` over 10k items in `onentry`: each `FE_NEXT` charges fuel. On exhaustion the continuation is `(ENTER, cur=s, sub=k, pc=FE_NEXT, iter[0]=handle, iter_idx[0]=4711)`. On resume, the same datamodel iterator continues. Shallow-copy semantics make concurrent mutation of the array irrelevant.

**10. Suspension during eventless processing, with `_event` observable.** Event E is dequeued, pinned and bound. Eventless microsteps whose conds read `_event.data` yield mid-chain. On resume, `cur_event` is still E (pinned) and the datamodel binding is untouched. Nothing re-binds, and E stays current until the next dequeue or stability.

**11. Internal event replaces `_event`.** During E's macrostep, `RAISE x` appends to the internal queue in the workspace. When the eventless set is empty, `DEQ_INTERNAL` releases E (freed if `refs` is 0, with payload released), pins x, and calls `bind_event`. Conditions and content now see x, and x stays current through any yields until the next dequeue or stability.

**12. 100k mostly-idle actors, 64 workspaces.** Idle actors are 64 B each with no workspace and no pinned events. A burst of 500 events to 500 actors:
- The first 64 acquire workspaces; the rest queue in `ws_wait`.
- A typical macrostep is under one quantum, so each actor reaches stability and frees its workspace and slot within a single turn.
- The pool cycles thousands of times per `ta_run`.

Peak event-pool use is mailboxes plus unstable actors. It doesn't grow with the 100k count.

---

## 19. Spec corner-case checklist (enforced by compiler and core)

- Transition priority:
  - Document order of atomic states, descendant-first.
  - Conflict removal with descendant-source preemption.
  - Cond results are memoised per selection pass; conds should be side-effect-free, and this changes only the evaluation count.
- Targetless transitions have no exit or entry. An internal transition whose targets aren't all descendants of its source behaves as external.
- History:
  - Record before *any* onexit.
  - Deep history stores atomic descendants, shallow stores the active child.
  - Default history content runs only when nothing is stored.
  - History-target transitions use a dynamic domain.
- Default initial: `<initial>` transition content runs after the parent's onentry. Multiple initial targets across parallel regions are supported.
- Done events:
  - Entering a final state raises `done.state.<parent>` with that final state's donedata.
  - A parallel is done when every region has an active final *child*.
  - A top-level final runs exit-interpreter (all onexit in reverse document order, cancel invokes, then `done.invoke` to the parent). Anything left in the queue is discarded.
- Data binding:
  - Early binding initialises all data at start.
  - Late binding initialises on first entry, but the variables exist from the start.
  - A data error raises `error.execution`; the document continues and the variable is unbound.
- Errors:
  - Errors in executable content abort only the current block.
  - Cond errors are treated as false and raise `error.execution` (plus the `<if>` policy flag from §1).
- Send: values are evaluated at send time. Invalid target or type is `error.execution`; unreachable is `error.communication`.
- `_event`:
  - `type` is `platform`, `internal` or `external`.
  - `origin`/`origintype` are set only for I/O-processor events.
  - `invokeid` is set on events from children.
  - `_event` is unbound before the first event.
- `<cancel>` affects only the canceller's own sends; cancelling an already-sent event is a no-op.
- `<finalize>` runs before selection; the compiler rejects `raise`/`send` inside it.
- Invoke: a stale child's events are dropped; invokes in states that don't survive the macrostep never start; autoforward is an exact copy.
- System variables are read-only (datamodel enforced, compiler pre-checked).
- Event descriptor matching is token-prefix: `foo.*` is the same as `foo`, and `*` matches everything.

The SCXML 1.0 IRP test suite should be the acceptance gate. Several items above (cancelled-child `onexit`, `<if>` cond errors, autoforward of the child's own events) are exactly where the spec leaves room, so each one should be pinned down by a test before it ships.

---

## 20. What to build first

1. Compiler front end to Chart IR, plus a dumper. Write the preorder, closure and domain precomputation and property tests against a naive reference interpreter.
2. `ta_core` with the null datamodel and a ws-provided internal ring. Fuzz it with random charts, differential against the reference.
3. `ta_system`: pool, mailboxes, scheduler, timers, I/O processor, invoke. Add deterministic replay tests with simulated time.
4. `ta_expr` arena datamodel, which makes the fully-POD system possible, then QuickJS for the ECMAScript profile and the IRP tests.
5. Bindings in order of ABI stress: WASM/Node (handles under 2⁵³, views into linear memory), then Python (payload capture/refcounting), then the Postgres extension (memory contexts, DSM with `ta_expr`).

The two riskiest decisions to validate early are the static-closure disjointness argument from §3 (fuzz it) and the release-at-stability `_event` rule from §9 (check it against the W3C `_event` tests).

I can turn this into a Claude Doc spec for the Tinyactors docs, or draft `ta.h` and the Chart IR section layout as real headers.# Proposed Solution
