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
## Proposed Solution
The right architecture is a **resumable SCXML semantic kernel over immutable compiled charts, with a separate library-owned actor runtime**. Hosts provide datamodels, external services, memory, and time. They should never implement transition selection, internal queues, invocation lifecycle, or SCXML addressing.

Three requirements need qualification:

- **Bounded memory cannot accommodate arbitrary queues, payloads, dynamic charts, or actor creation.** The runtime needs explicit admission limits and observable resource exhaustion.
- **Cooperative fuel cannot preempt arbitrary Python, JavaScript, C, or other datamodel code.** A strict return-time guarantee requires bounded callbacks or an interruptible datamodel implementation.
- **Pooling execution workspace does not eliminate unfinished execution state.** Every suspended actor must retain its continuation somewhere. A few permanently runnable actors can share a few workspaces; arbitrarily many suspended actors cannot.

“Most performant” is also workload-dependent. The design should offer a compact baseline and a few compiler-selected optimizations, with measured tradeoffs rather than one universally maximal representation.

The following is a proposed architecture, checked against the [SCXML Recommendation](https://www.w3.org/TR/scxml/) and its [errata](https://www.w3.org/2015/08/scxml-errata.html). Its representations, resource policies, scheduling scheme, and ABI are design proposals.

**1. Architectural boundaries**

Keep the four proposed layers, but make their dependency direction strict:

| Layer | Owns | Does not require |
|---|---|---|
| `ta_core` | One interpreter’s semantic phases, configuration, history, executable-content VM, event selection and `_event` binding | XML parser, actor scheduler, OS services |
| `ta_system` | Sessions, queues, scheduling, timers, local routing, invocation, resource pools, effect protocol | Threads, sockets, filesystem, OS clock |
| `ta_compile` | XML parsing, validation, datamodel compilation hooks, Chart IR and System IR generation | Presence in the deployed runtime |
| `ta_runtime` | Optional event-loop adapters, resource fetching, transport adapters, allocator conveniences | Presence in constrained deployments |

`ta_core` depends on a small internal service interface for event storage, dispatch, invocation, and effects. `ta_system` supplies that interface.

For independent kernel use, supply a library-provided single-session implementation of those services. This is essential: “independent kernel” must not become “every binding writes its own SCXML queue implementation.”

The kernel and system are separate build targets, but the ordinary embedding API is `ta_system`.

A single actor can therefore run through the same API as a packaged actor system. Thin bindings do not need a second semantic implementation.

**2. Invariants that determine the design**

Use these invariants as implementation and verification obligations:

1. An actor processes at most one semantic execution continuation at a time.
2. Enqueueing an event never executes its recipient recursively.
3. Scheduler suspension never causes event dequeueing, rebinding, invocation startup, or state entry by itself.
4. External events are consumed only at the appropriate stable boundary.
5. Transition selection completes before its selected transitions mutate the configuration.
6. Executable content resumes without repeating completed expressions or effects.
7. Queues, timers, effects, and current-event bindings hold explicit ownership references.
8. Actor handles cannot silently become references to replacement actors.
9. Every potentially large runtime loop has a resumable cursor and consumes fuel.
10. Resource exhaustion never silently drops an accepted semantic operation.

Run-to-completion is consequently an **actor-local ordering guarantee**, not a requirement to occupy the native thread until a macrostep finishes.

Other actors may execute while one actor is suspended inside its macrostep. That actor still cannot consume its next external event prematurely.

**3. Packed Chart IR**

The chart is an immutable byte image. It contains no runtime addresses, language objects, or native function pointers.

A possible section directory is:

```text
ChartHeader
SectionDirectory
StateTable
TransitionTable
TargetLists
ChildLists
InitialAndHistoryDescriptors
ActionBlocks
ActionBytecode
SendDescriptors
InvokeDescriptors
DataInitializationDescriptors
ExpressionDescriptors
EventMatcherTables
StringAndLiteralBytes
OptionalAccelerationTables
OptionalSourceMap
```

The header contains:

```text
magic
image_format_version
required_feature_bits
byte_length
chart_fingerprint
state_count
transition_count
expression_count
layout_profile
section_directory_offset
```

Each section has a byte offset, count, stride or encoding identifier, and required alignment.

**Use byte offsets for the image and integer IDs for graph relationships.** Do not confuse a chart-local state ID, a system event-name ID, and an ABI actor handle.

A compact state record can contain:

```text
parent
subtree_end
kind_and_flags
children_range
transitions_range
onentry_blocks_range
onexit_blocks_range
initial_descriptor
history_descriptors_range
invoke_descriptors_range
data_initialization_range
name_string_id
```

Assign ordinary state IDs in preorder. A state’s descendants then occupy a contiguous interval. Ancestor tests become interval comparisons, and forward/reverse scans naturally support entry/exit traversal.

Initial and history pseudo-states need their own descriptors. They should not consume active-configuration bits as though they were ordinary active states.

A transition record contains:

```text
source_state
event_matcher_id_or_eventless
condition_expression_id_or_none
targets_range
domain_descriptor
action_block_id
flags
```

A domain descriptor is either:

- a compiler-proved constant domain; or
- a compact recipe requiring runtime effective-target resolution.

History is the important reason not to promise a constant domain for every transition. Its effective targets depend on saved configuration. The compiler should prove constancy when possible and retain a correct fallback otherwise.

**Precompute selectively:**

| Information | Baseline representation | Optional faster representation |
|---|---|---|
| Descendancy | Preorder interval | Ancestor masks |
| Ancestor traversal | Parent IDs | Flattened ancestor lists |
| Exit region | Domain interval | Cached masks |
| Initial expansion | Compact entry recipe | Flattened static plan |
| Event candidates | Descriptor index plus ordered lists | Cached candidate spans |
| State membership | Configuration bitset | Small-chart machine word |
| Source priority | Hierarchy and document order | Sparse comparison metadata |

Do not generate a transition-by-transition conflict matrix by default. Its quadratic size is inconsistent with memory conservation.

Likewise, do not generate every possible configuration’s entry plan. Parallel states and history can make that space enormous.

**Ownership and loading.** The embedder owns the image bytes and keeps them alive until all referencing actors and views are gone. A runtime chart view points into those bytes; actors store a chart index rather than duplicate the structure.

**Portability.** Define an endian format and decode fields through accessors. Never cast arbitrary file bytes to packed C structs. Alignment, effective-type rules, and unaligned access make that unsafe.

Matching-endian, suitably aligned sections can have specialized accessors. Harvard-architecture flash may require a platform read hook. Position independence does not imply that every processor can read flash with ordinary RAM loads.

The loader validates section bounds, multiplication overflow, IDs, branch destinations, instruction boundaries, and feature requirements before execution.

A single relocatable blob is feasible. Universal native datamodel bytecode is not; keep portable expression descriptors in the blob and bind platform-specific compiled expression artifacts separately.

**4. Compiled System IR**

System IR packages charts and startup topology without pretending that all future actors are statically known.

```text
SystemHeader
ChartDirectory
RootActorTemplates
DatamodelRequirements
ResourceDirectory
InvocationResourceMappings
ExternalProcessorRequirements
StartupParameters
OptionalDebugMetadata
EmbeddedChartBlobs
```

An actor template specifies:

```text
chart_index
datamodel_binding_index
startup_parameter_descriptor
optional_application_name
scheduling_class
```

Resource mappings resolve chart URIs and embedded invocation content to compiled chart indices. Preserve base-URI information needed by resource resolution.

Avoid compiling arbitrary application wiring into a second actor language. Add only what is necessary for roots, packaged resources, and deployment configuration.

SCXML invocation still creates fresh runtime sessions. A template is not a singleton instance.

Dynamic `srcexpr`, externally obtained markup, and datamodel-generated invocation content require a resource-resolution path. That path may:

1. resolve to an existing packaged chart;
2. obtain a compiled image from a host-side compiler service; or
3. use an optional compiler linked into the deployment.

Normal execution still never traverses XML.

A closed-image deployment can reject resources outside its declared catalog, but it must advertise that restriction. It cannot claim to execute every possible dynamic SCXML document.

**5. Mutable instance layout**

Separate three quantities:

```c
ta_instance_size(chart, profile);
ta_instance_align(chart, profile);
ta_workspace_size(chart, profile);
```

The persistent instance contains only state that remains meaningful between semantic operations:

```text
lifecycle flags
current-event binding handle
datamodel context handle
active configuration
initialized-data markers, when needed
history-valid markers
history values
pending-invocation markers
invocation slot references
generated-ID sequence state
optional compact continuation or spill reference
```

System-specific queue and scheduling fields belong in the actor directory, not in the chart or duplicated kernel structures.

Let:

- `S` = ordinary state count;
- `D` = number of state-local data initialization groups;
- `H` = number of history descriptors;
- `C_h` = encoded capacity for history descriptor `h`;
- `I` = maximum simultaneously active invocation slots.

A baseline persistent cost is:

```text
instance_header
+ ceil(S / 8)
+ optional ceil(D / 8)
+ ceil(H / 8)
+ sum(C_h)
+ invocation-slot references
+ alignment
```

History capacity is chart-dependent:

- shallow history of an ordinary compound state can often be one child ID;
- shallow history of a parallel state may require several child IDs;
- deep history stores remembered atomic descendants;
- a sparse ID vector can be smaller than a full subtree bitset.

Do not allocate a full-chart bitset for each history descriptor.

For a tiny chart, configuration may occupy one machine word. For larger charts, a bitset is a simple baseline. A compiler-selected active-leaf representation can save memory when the maximum active frontier is small, but `In()` and mid-entry/mid-exit membership must remain correct.

An illustrative small-chart header might occupy 32–64 bytes. That is an engineering target, not a guaranteed ABI size. Datamodel contexts and retained payloads are additional memory.

**6. Actor and system layout**

The system uses packed arrays and pool indices:

```text
ActorDirectory
InstanceArena
RunnableQueue
EventEnvelopePool
QueueNodePool
TimerPoolAndHeap
InvocationPool
EffectPool
ContinuationAndWorkspacePools
DynamicNamePool
SessionRegistry
```

A slot in the actor directory contains:

```text
generation
chart_index
instance_offset
datamodel_context_handle
internal_queue_head/tail
external_queue_head/tail
runnable_next
continuation_reference
parent_invocation_reference
owned_timer_list_head
status_and_flags
```

An idle actor has no allocated mailbox buffer. Empty queues are sentinel indices.

Use slot-plus-generation handles publicly. On generation exhaustion, retire a slot or widen the generation scheme; wrapping into a still-valid stale handle is unacceptable.

Give `_sessionid` a separate, stable textual representation. A public session address and an internal actor handle are related but are not interchangeable ABI types.

The session registry resolves textual addresses. A generated local address can encode a directly decodable handle, while any accepted aliases use a bounded lookup table.

Actors own their persistent instances. The system owns pool slots and relationship records. A completed actor may retain a diagnostic tombstone if requested; otherwise destruction releases its resources according to the lifecycle policy.

No threads are required. A system has one execution owner. Thread-safe posting is an optional adapter that feeds the same ordered ingress path.

**7. Event representation and pool**

Separate an event’s semantic envelope from membership in a queue.

```text
EventEnvelope:
    name_handle
    event_kind
    sendid_handle
    origin_handle
    origintype_handle
    invokeid_handle
    payload_handle
    payload_domain
    ownership_count
```

```text
QueueNode:
    event_handle
    next_node
```

`event_kind` and queue class are independent. A platform error event, for example, can reside in an internal queue.

Separating envelopes and queue nodes allows several queues to reference an immutable envelope when semantically legal. An envelope cannot contain its only queue link if it may belong to multiple queues.

A reference may be held by:

- a queue node;
- a delayed-send record;
- an actor’s current-event binding;
- an outbound effect;
- an inspection pin;
- a datamodel object that retains event backing storage.

Pool entries use indices and generation checks where stale references can escape.

**Payloads remain datamodel-native.** `payload_handle` is an opaque token interpreted by a payload/datamodel adapter. The kernel does not know whether it represents a Python object, Ruby value, PostgreSQL datum, or arena index.

The adapter provides operations such as:

```text
retain
release
snapshot_for_send
import_for_receiver
optional export_for_external_transport
```

Retention alone is insufficient for communicating actors. The runtime must establish payload independence between sender and receiver. A datamodel can implement that through copying, immutable values, or copy-on-write. Passing the same mutable Python dictionary to both actors is incorrect. [SCXML I/O processor](https://www.w3.org/TR/scxml/#SCXMLEventProcessor)

The local path therefore avoids serialization and application-level roundtrips, but may still invoke payload adapter code. Eliminating all language-runtime participation is incompatible with arbitrary native values.

Illustrative pool costs are 40–64 bytes per general envelope and 8–16 bytes per queue node, excluding payload storage. A compact embedded profile can reduce field widths. ABI layouts should not expose these internals.

**8. `_event` lifetime model**

Treat the currently bound event as persistent instance state.

At initialization, the binding is absent. On dequeue for semantic processing:

```text
queue reference becomes current-binding reference
publish new binding to datamodel
release previous binding after publication
```

Do not clear the binding at a scheduler yield or at stability. `_event` remains bound until another event is processed. [System variables](https://www.w3.org/TR/scxml/#SystemVariables)

The state changes are:

| Execution circumstance | Current binding |
|---|---|
| Initialization before first event | Absent |
| Processing an external event | That external event |
| Following eventless transitions | Unchanged |
| Quantum exhaustion | Unchanged |
| Reaching stability | Unchanged |
| Dequeueing an internal event | Replaced by that internal event |
| Processing the next external event | Replaced again |
| Session teardown | Released |

An ignored event still becomes current if it was dequeued for processing.

This creates a significant memory consequence: **100,000 idle actors may retain 100,000 last events and their payloads.** Workspace pooling cannot remove that semantic state.

There are two valid implementations:

- retain the pooled event envelope as the binding;
- transfer the semantic event into an actor-owned datamodel representation and release the envelope.

The second saves envelope slots, not necessarily payload memory.

Lazy event wrappers must pin their backing envelope. A view borrowed for one callback must never be stored in a language object without acquiring ownership.

If application code can retain an old event object, its storage needs an independent datamodel lifetime after the interpreter replaces its own binding.

**9. Event names and matching**

Use integer names internally, but do not reduce matching to integer equality.

The image provides:

- immutable event-name bytes;
- normalized descriptor token paths;
- descriptor-to-transition indexes;
- precompiled names for generated state-completion events.

The system provides a bounded dynamic-name interner for incoming and expression-generated names.

Compile descriptor matching into a compact token-prefix trie or equivalent table. Match an incoming name once to descriptor classes, then obtain state-local candidate spans in document order.

For repeated names, cache those spans using chart identity and name generation. Bound the cache independently of actor count.

A descriptor such as `error` matches the relevant token-prefix family, not arbitrary character prefixes. Case remains significant. Normalize the specified trailing wildcard forms at compilation. [Event descriptors](https://www.w3.org/TR/scxml/#EventDescriptors)

Do not assign each arbitrary dynamic name a permanent global ID. Reference-count or evict dynamic names when envelopes, timers, and bindings no longer need them.

Bound input string length. Tokenization, comparison, and hashing of unusually large names must either consume fuel in chunks or happen through a bounded ingress operation.

For known event handles, the common path is:

```text
name handle
→ cached descriptor matches
→ ordered state-local candidates
→ condition evaluation
```

Avoid flattening every ancestor transition list into every descendant. That trades substantial image duplication for a benefit that many charts do not need.

**10. Mailboxes and event ordering**

Each actor has two FIFO queues:

```text
internal: head, tail
external: head, tail
```

The external queue is its mailbox. Introducing another mailbox layer usually adds unnecessary nodes and ordering questions.

Appending and removing nodes are constant-time pool operations.

Events arriving while the actor is suspended append normally. They do not interrupt its current executable block or macrostep.

The semantic pump follows this structure:

```text
complete current semantic work

try eventless transitions
if selected:
    execute a microstep
    repeat

if internal queue is nonempty:
    bind and process its next event
    repeat

start eligible pending invocations
if invocation startup generated internal work:
    repeat

reach external-event boundary
if external queue is nonempty:
    bind next external event
    perform invocation preprocessing
    select transitions
else:
    become idle
```

This is a resumable control flow, not one unbounded loop invocation.

Internal queues retain FIFO ordering. Errors are appended; they do not leap ahead of previously raised events.

The queue pool determines memory use:

```text
O(queued deliveries)
```

rather than:

```text
O(actor count × reserved mailbox capacity)
```

Per-actor queue quotas can prevent one actor from consuming the entire shared pool.

**11. Transition selection, exit, and entry**

Transition selection visits active atomic states in document order. For each, inspect its own transitions and then its ancestors’ transitions, preserving each state’s transition order.

Store candidates in first-selection order and deduplicate selected transition IDs.

Do not replace this with “take the lowest global transition ID.” Selection order, conflict priority, and action execution order are distinct concerns.

After selection, compute conflicts from actual exit sets. Prefer the descendant source when required, otherwise preserve the appropriate earlier selection. Targetless transitions need no exit mask.

For a constant domain:

```text
exit set = active configuration ∩ proper-descendant interval
```

Nested preorder intervals can often reduce a conflict test to a query for active states in their intersection. Small charts can use direct word masks.

For history-dependent cases, resolve effective targets against the relevant saved history before deciding the domain.

Workspace holds:

```text
selected transition IDs
conflict-filter cursors
resolved domain data
exit set
entry set
default-entry markers
default-history action markers
entry-expansion worklist
```

Execute:

```text
history capture
state exits
transition actions
state entries
```

Capture needed history against the configuration before destructive exit processing.

Exit processing preserves handler order, invocation cancellation, and membership removal. Entry processing preserves membership insertion, first-entry data initialization, onentry blocks, initial/default-history content, and completion-event generation.

The mutable configuration may temporarily represent a partially completed entry or exit sequence. `In()` must report the state at the current semantic point. The debugger should mark such snapshots as mid-operation rather than pretend they are stable configurations.

Use forward/reverse ordered scans and a bounded explicit expansion worklist. No hierarchy recursion is necessary.

Memory is linear in chart size or maximum frontier, depending on the chosen representation. Execution cost is proportional to examined candidates, active regions, and changed states—not XML size.

**12. Continuation machine**

The actor continuation needs more than one bytecode PC, but far less than a native stack.

A useful phase set is:

```text
INITIALIZE
EXTERNAL_PREPROCESS
SELECT
FILTER_CONFLICTS
CAPTURE_HISTORY
EXIT
TRANSITION_ACTIONS
BUILD_ENTRY
ENTER
EVENTLESS_OR_INTERNAL
START_INVOCATIONS
TERMINATE
```

Executable content runs under its enclosing phase.

A continuation contains:

```text
phase
subphase and flags
outer-loop cursor
inner-loop cursor
action PC
current action-block identifier
workspace/spill reference
pending request reference
```

Chart-specific workspace retains selected sets, expansion tasks, live foreach frames, and partially built operation operands.

Do not create a phase for every field assignment. Process small bounded chunks and save cursors at boundaries.

The central rule is:

> Resume after completed semantic work; never reconstruct it by repeating expressions or effects.

Rebuilding derived masks can be safe. Re-running a completed condition, payload expression, script, or send is not a general recovery mechanism.

**Workspace pooling requires two kinds of storage:**

1. scratch that can be discarded or recomputed without observable work;
2. live continuation data that must survive suspension.

Stable actors require neither, apart from persistent invocation/history/current-event state.

A suspended actor either retains its workspace or spills the live portion to a continuation pool. Scratch can then be reused.

If the continuation pool is full, report a resource condition. Never make a runnable actor appear idle merely because it lacks storage.

For memory-poor profiles, prefer yields at clean semantic boundaries when possible. Still retain mid-operation support for large charts, long executable blocks, and large iterations.

**13. Runnable scheduler and fuel**

Use a FIFO runnable queue with an intrusive next index in each actor slot. A queued flag prevents duplicate insertion.

Actor lifecycle and scheduling status should be separate:

| Status | Meaning |
|---|---|
| `IDLE` | At an external-event boundary with no available event |
| `RUNNABLE` | Has semantic work or an event it may consume |
| `WAIT_REQUEST` | Continuation needs a particular host response |
| `RESOURCE_WAIT` | Progress needs additional runtime capacity |
| `DONE` | Normal semantic termination |
| `FAULTED` | Runtime cannot safely continue |

Give each dispatch an actor quantum and each public run call a system budget.

```text
while budget remains:
    choose due timer work or runnable actor

    if actor:
        grant min(actor_quantum, remaining_budget)
        advance its continuation
        subtract actual work

        if still runnable:
            append actor to queue tail
```

Timer dispatch, actor creation, effect construction, and other system work also consume the system budget. A timer storm must not bypass the limit.

Charge work for bounded units:

```text
candidate examination
condition call
bitset word processing
state-entry/exit stage
bytecode instruction
iteration advance
queue operation
bounded timer-heap operation
bounded copy/name-processing chunk
```

A bulk operation must not hide arbitrary work behind one fuel unit.

A strict implementation can require a minimum usable budget or split primitives further; avoid a “one unit” operation that cannot run under the advertised quantum.

For `R` continuously runnable actors, round-robin scheduling gives each a dispatch after at most approximately `R−1` other actor dispatches, provided callbacks terminate and system-service scheduling is also fair.

This is fairness in deterministic work units. It is not a wall-clock guarantee.

Changing actor quantum may change cross-actor interleavings and thus message order. Scheduler suspension preserves actor-local semantics; quantum-independent whole-system behavior is a stronger property this design does not promise.

**14. Executable-content bytecode**

Use a structural VM, not a universal expression VM.

An illustrative instruction set is:

```text
ASSIGN          assignment_descriptor
BRANCH_FALSE    condition_id, destination
JUMP            destination
FOREACH_BEGIN   iteration_descriptor, end_pc
FOREACH_NEXT    frame_slot, body_pc
RAISE           event_name_id
SEND            send_descriptor
CANCEL          cancel_descriptor
LOG             log_descriptor
SCRIPT          expression_or_script_id
CUSTOM          extension_descriptor
END_BLOCK
```

Keep large operands and action-specific details in descriptor tables. A word-based encoding with short common instructions is simpler to decode and validate than aggressively variable-length instructions.

Flatten `if` into branches. A `foreach` needs a live iteration frame containing:

```text
datamodel iterator/snapshot token
iteration position
loop descriptor
cleanup state
```

Its body is still ordinary bytecode.

The datamodel owns the collection snapshot needed to preserve iteration behavior under mutation. A native iterator over a changing collection is insufficient. [Foreach](https://www.w3.org/TR/scxml/#foreach)

Nested loops require storage proportional to live nesting depth. The compiler determines the maximum structural depth, but the datamodel’s snapshot memory depends on collection size.

Compile error-unwind destinations for executable blocks. On failure, clean up live frames and temporary values, enqueue the appropriate error, and skip the remaining affected block. Avoid treating every error as actor termination.

`<invoke>` is a state-associated descriptor processed at the invocation phase, not simply an ordinary onentry opcode.

Before yielding, preserve:

- the next instruction;
- evaluated operands that must not be evaluated again;
- pending payload builders;
- loop frames;
- any external request identity.

Effects acquire unique operation identities before publication so a resume cannot emit them twice.

**15. Datamodel interface**

Keep synchronous semantic services distinct from the external-effect protocol.

The kernel needs these operations:

| Operation | Purpose |
|---|---|
| Create/destroy context | Datamodel instance lifecycle |
| Initialize data group | Early/late binding and invocation inputs |
| Evaluate condition | Boolean semantics |
| Evaluate typed operand | Name, URI, duration, identifier, or opaque value |
| Assign | Location semantics and mutation |
| Execute script | Selected datamodel scripting |
| Begin/advance/end foreach | Snapshot and loop-variable semantics |
| Build payload | Content, params, namelist, donedata |
| Import payload | Recipient representation |
| Retain/release opaque token | Lifetimes |
| Publish system-variable binding | `_event`, session metadata, processor metadata |
| Optional resumable operation | Bounded or cooperative datamodel execution |

A representative interface could use:

```c
ta_dm_status test(ctx, expr_id, semantic_view, bool *out);
ta_dm_status assign(ctx, assignment_id, semantic_view);
ta_dm_status operand(ctx, expr_id, expected_kind, ta_operand *out);

ta_dm_status iter_begin(ctx, loop_id, semantic_view, ta_dm_token *out);
ta_dm_status iter_next(ctx, ta_dm_token, bool *has_item);
void         iter_end(ctx, ta_dm_token);

ta_dm_status payload_build(ctx, payload_descriptor, ta_dm_token *out);
ta_dm_status script(ctx, script_id, semantic_view);
```

`ta_operand` contains only ABI-relevant primitives and opaque tokens. It is not a general value algebra.

The datamodel builds any required native object structure. Preserve ordered parameter pairs and duplicates until the selected datamodel or processor interprets them; an intermediate hash map would lose information.

Expression IDs index chart-local descriptors. A binding maps them to compiled code, closures, or native switch cases through a shared binding artifact. Instances do not duplicate compiled expressions.

**System-variable control.** The core supplies an immutable semantic view containing:

```text
current event or absent
session identity
chart name
I/O processor descriptors
state-membership query
platform variables under _x
```

The adapter exposes that view through the datamodel’s prescribed representation and enforces protected bindings. The application never manually synchronizes `_event`.

Do not let scripts mutate the active configuration directly.

The errata clarify the datamodel’s expression-language requirements, including side-effect-free boolean expressions and `In()`. The adapter must uphold those requirements rather than let a generic host-language evaluator define accidental semantics. [Datamodel errata](https://www.w3.org/2015/08/scxml-errata.html#E2)

**Fairness boundary.** A synchronous operation can be arbitrarily slow. For strict bounded execution, require one of:

- a bounded native datamodel;
- instruction-limited expression/script execution;
- resumable datamodel operations with saved tokens.

The last option introduces datamodel continuation memory. There is no portable C mechanism that can safely interrupt an arbitrary foreign-language callback.

**16. Built-in SCXML I/O processor**

Compile literal addresses into semantic target descriptors:

```text
SELF_INTERNAL
SELF_EXTERNAL
PARENT
INVOKED_SESSION(invoke descriptor or ID)
SESSION(session identity)
EXTERNAL_URI(uri ID)
DYNAMIC_TARGET(expression ID)
```

Resolve dynamic strings once when evaluating the send. The runtime—not a binding—recognizes special SCXML address forms.

Local dispatch:

```text
evaluate send operands
construct isolated payload
resolve target
create event envelope and queue node
append to destination queue
mark destination runnable when appropriate
continue sender
```

No recursive actor execution occurs.

Keep address resolution separate from event metadata construction. Origin, send ID, invocation identity, and processor type have their own semantic rules.

Omitted target means self-external. Internal-target sends use the internal queue. Parent and invoked-session addressing use relationship records.

A recognized local session address that is absent or inaccessible is a communication error; do not forward every failed local lookup to the host as an arbitrary external URL.

An external address or supported external processor produces an effect record. Unsupported processor types and invalid addresses follow their respective semantic error paths.

Host policy determines what is accessible beyond the packaged system, while the library performs all local SCXML interpretation of addresses.

**17. Time, delayed sends, and cancellation**

Use explicit monotonic logical time:

```text
uint64_t ticks
declared tick unit
checked duration conversion
```

The host advances time. The core never samples an OS clock.

A delayed-send record contains:

```text
deadline
insertion_sequence
sender_handle
sendid_handle_or_none
captured_target
event_or_payload_reference
heap_position
sender_owned_list_links
```

Use an indexed binary min-heap ordered by `(deadline, insertion_sequence)` as the baseline. It is compact, exact, and predictable. A timing wheel is an optional workload-specific optimization.

Send operands and payload are captured at send execution, not reevaluated at dispatch. Sender termination removes its pending delayed sends. [Send](https://www.w3.org/TR/scxml/#send)

Do not implement delayed internal-target sends as an extra convenience; respect the SCXML restrictions.

Cancellation is scoped to the sending session. Maintain a bounded index from `(sender, sendid)` to the pending records. Reused IDs may identify several delayed sends, so cancellation cannot assume uniqueness.

Normal state exit does not automatically cancel unrelated delayed sends. Session termination does.

At expiration, revalidate destination existence and invocation accessibility. Never deliver to a replacement actor occupying a reused slot.

Large cancellation sets and large batches of expired timers need resumable traversal. Only a bounded amount of timer work occurs in one run budget.

`advance_time()` can merely update time and make timer work eligible. `next_deadline()` lets the host integrate with asyncio, libuv, mobile event loops, or simulation.

Define cancellation-versus-expiration order by the sequence of API calls and scheduler operations. Deterministic ties are runtime policy, not an inferred universal SCXML ordering rule.

**18. Invocation machinery**

An invocation record contains:

```text
parent_actor
owning_state
invoke_descriptor
invokeid
child_actor_or_external_service_handle
generation
lifecycle
autoforward_flag
finalize_block
```

Entry marks invocations pending. Startup occurs only after the actor’s relevant internal stabilization work. An entered-and-immediately-exited state must not start a service unnecessarily. [Invoke](https://www.w3.org/TR/scxml/#invoke)

For packaged SCXML invocation:

```text
resolve compiled chart
evaluate and isolate startup parameters
allocate fresh actor/session
establish parent relationship
initialize child continuation
enqueue child as runnable
```

The parent does not execute the child inline.

For external invocation, publish an invocation-start effect and maintain the same relationship semantics around the host service handle.

Invocation preprocessing runs at the parent’s external-event processing boundary:

- identify the producing invocation;
- execute its finalize block when applicable;
- perform autoforward behavior;
- proceed to ordinary transition selection.

Do not model autoforward as another `<send>` that invents new event metadata.

Child completion creates `done.invoke.<invokeid>` for the parent’s external queue. Child state completion remains distinct from invocation completion.

Exiting the invoking state cancels the invocation after its exit handlers. A child cancellation uses a control flag, not an ordinary event that can wait behind an infinite macrostep. Observe it at the required microstep boundary and execute teardown handlers cooperatively.

Cancellation and ordinary completion must remain distinct. Generation-qualified invocation records also suppress stale service responses.

A child stuck inside an uninterruptible script exposes the same datamodel limitation as scheduling: prompt cooperative cancellation requires a cooperative datamodel.

Memory is proportional to live invocations, not every possible invocation template.

**19. External effects and requests**

Distinguish three protocols:

| Protocol | Actor behavior |
|---|---|
| Emission | Continues after the runtime accepts the effect |
| Asynchronous service/event | Continues; responses arrive as future events |
| Blocking executable request | Suspends that actor’s current continuation |

Ordinary SCXML sends and invocations must not automatically become blocking awaits.

Use blocking requests for explicitly defined custom executable content, runtime resource resolution where necessary, or an approved asynchronous semantic adapter.

A request contains:

```text
request_id
actor_handle
operation_kind
input references
expected_response_kind
continuation identity
cancellation/lifecycle state
```

Publish it once, mark the actor `WAIT_REQUEST`, and continue scheduling other actors.

The host later supplies `(request_id, result)`. Validate identity and lifecycle, install the result, and mark the actor runnable. It resumes the unfinished operation before consuming another event.

Effect records contain immutable borrowed views and opaque payload tokens. Draining an effect batch does not itself release those records; explicit acknowledgment does.

For deferred transport failures, correlate the report with the original operation and sender. A fire-and-forget effect does not need a domain response to unblock the actor, but may still need dispatch acknowledgment and failure reporting.

Backpressure is a resource condition. Returning to the host for more effect capacity is permitted, provided the continuation preserves what has already happened and never retries by re-emitting the same effect.

**20. Public ABI**

Keep image format, internal C implementation, and public ABI independently versioned.

An illustrative system API is:

```c
ta_status ta_image_open(
    const void *bytes, size_t length,
    const ta_image_options *options,
    ta_image **out);

ta_status ta_system_requirements(
    const ta_image *image,
    const ta_limits *limits,
    ta_memory_requirements *out);

ta_status ta_system_init(
    void *memory, size_t bytes,
    const ta_image *image,
    const ta_system_options *options,
    ta_system **out);

ta_status ta_system_send(
    ta_system *system,
    ta_actor actor,
    const ta_event_input *event);

ta_status ta_system_send_many(
    ta_system *system,
    const ta_delivery_input *items,
    size_t count,
    size_t *accepted);

ta_run_result ta_system_run(
    ta_system *system,
    uint64_t budget);

ta_status ta_system_resume(
    ta_system *system,
    ta_request request,
    const ta_response *response);

ta_status ta_system_advance_time(
    ta_system *system,
    uint64_t now);

ta_status ta_system_effects(
    ta_system *system,
    ta_effect_view *out,
    size_t capacity,
    size_t *written);

ta_status ta_system_ack_effects(
    ta_system *system,
    const ta_effect_id *ids,
    size_t count);

ta_status ta_system_inspect(
    const ta_system *system,
    const ta_inspection_query *query,
    ta_inspection_output *out);

void ta_system_destroy(ta_system *system);
```

`ta_image_open()` itself can use caller-provided view storage; the signature above is illustrative.

A run result should provide both a primary stop reason and orthogonal state:

```text
stop reason:
    IDLE / YIELDED / EFFECTS / NEED_MEMORY / DONE / FAULT

additional fields:
    work_used
    runnable_count
    waiting_request_count
    available_effect_count
    next_deadline_or_none
```

An idle system may have pending timers or service responses. One actor completing does not mean the system is done.

Use fixed-width integers, explicit tags, caller-sized structures, versioned vtables, and output pointers. Avoid bitfields, exposed internal structs, C `long`, varargs, and ABI-sensitive large struct returns.

Ownership rules should be explicit:

| Input/view | Lifetime |
|---|---|
| Image bytes | Until image and referencing systems are closed |
| Input string bytes | Borrowed during call; copied/interned if accepted |
| Input native payload | Retained/imported if accepted |
| Rejected input | Ownership stays with caller |
| Borrowed inspection data | Until its documented mutation boundary |
| Effect view | Until acknowledged or explicitly released |
| Request inputs | Until response/cancellation completion |
| Datamodel temporary view | Only during operation unless pinned |

For `send_many`, define accepted-prefix behavior. An all-or-nothing option may be useful, but requires preflight capacity checks.

Batch event submission, response submission, effect draining, and inspection. Ordinary local actor traffic stays entirely inside the runtime.

**21. Inspection and reconstruction**

Inspection should return IDs and borrowed tables, not serialize everything automatically.

Expose:

```text
actor lifecycle and semantic phase
active state IDs
current-event metadata
queue lengths
pending invocations
owned delayed sends
pending requests
resource usage and high-water marks
```

Optional source maps turn IDs into chart state names and source locations.

A snapshot of a yielded actor can be mid-exit or mid-entry. Include phase information so inspection does not imply stable configuration.

Provide cheap stable inspection and optional detailed continuation inspection. Datamodel value inspection is adapter-specific.

For reconstruction, distinguish:

- **stable checkpoint:** persistent actors, configuration, histories, queues, current events, timers, relationships, and datamodel state;
- **arbitrary suspended checkpoint:** all of the above plus continuations, loop snapshots, temporary values, and pending operations.

A full checkpoint requires datamodel export/import hooks. Raw Python pointers, PostgreSQL datums, or host closures are not portable snapshots.

External operations also need replay, reconnection, or invalidation policy. Restoring an old request must not perform its external effect twice.

Key checkpoints by chart fingerprints and expression-binding identity. Remap handles during restore. Raw memory dumps are not a stable serialization format.

**22. Memory allocation and failure behavior**

Offer two profiles over the same semantics:

- **fixed capacity:** caller supplies all runtime arenas and pools;
- **growable:** allocator hooks extend pools at explicit boundaries.

The fixed-capacity kernel makes no mandatory execution-time heap allocations. That guarantee excludes allocations performed by the selected datamodel unless its contract says otherwise.

A capacity manifest includes:

```text
actors
queued delivery nodes
live event envelopes
payload bytes or adapter quota
dynamic names and string bytes
timers
invocations
effects
requests
live continuations
workspace bytes
```

These quantities cannot generally be derived from chart size alone. Loops can produce unbounded messages, and external inputs can accumulate indefinitely.

For host ingress, reject before accepting ownership when capacity is unavailable.

For in-flight semantic work:

1. preserve a resumable resource wait when capacity can be added;
2. generate an appropriate semantic error where the operation’s rules permit it;
3. report a runtime fault if neither safe continuation nor required error reporting is possible.

Do not introduce arbitrary blocking local sends merely to hide a full event pool. That can change behavior or deadlock actors.

Reserve emergency capacity for error and lifecycle control. A reserve improves robustness but cannot guarantee progress for an arbitrary actor that endlessly handles errors by producing more errors.

Fuel guarantees CPU sharing. It does not guarantee memory sharing. Per-actor quotas and global admission limits address that separate problem.

**23. The twelve scenarios**

1. **Normal external event causing a transition.**  
   Ingress validates the actor, imports the payload, and appends an external queue node. At the actor’s external boundary, dequeue transfers ownership to its current binding. The matcher supplies ordered candidates; conditions select transitions. The kernel captures history, exits states, executes actions, enters states, processes subsequent internal work, and reaches stability. The last event remains bound.

2. **Eventless chain reaching stability.**  
   Eventless selection and microsteps repeat without dequeueing a new event. If no event has ever been processed, the current binding remains absent. Otherwise it remains the last processed event. Stability releases transient workspace and permits invocation startup or external dequeueing.

3. **One infinitely looping eventless actor.**  
   Each candidate examination, state operation, and action consumes fuel. Quantum exhaustion saves its continuation and places it at the runnable queue’s tail. It never needs another event to wake it. The public run call stops when its system budget is exhausted.

4. **Several infinitely runnable actors.**  
   Each receives a bounded dispatch in FIFO rotation. All retain their unfinished semantic state. No actor consumes its next external event during an unfinished macrostep. The host regains control after the aggregate budget.

5. **Actor A sends to actor B locally.**  
   A evaluates the send and establishes an isolated payload. The system resolves B’s session, appends B’s external queue, and makes B eligible to run. A continues its own action block. B executes only when scheduled.

6. **Invoked child sends or replies to its parent.**  
   The child uses its relationship record to resolve `#_parent`. The resulting parent event carries invocation attribution. When the parent processes it, the matching finalize block runs before transition selection. Parent-to-child sends resolve through the invocation registry. Normal child completion produces a separate completion event.

7. **Delayed cross-actor send.**  
   The sender captures operands and payload and creates a timer record. Time advancement makes it due. Budgeted timer processing resolves the still-valid destination and appends its queue. Cancellation removes matching sender-owned pending records; sender termination discards its remaining delayed sends.

8. **External asynchronous I/O suspends an actor.**  
   For an explicitly blocking extension, the VM creates one request and preserves its PC and operands. The actor becomes `WAIT_REQUEST`; other actors run. A matching host response makes it runnable, and it completes the same operation. Ordinary asynchronous send/invoke instead continues and receives later events.

9. **Suspension during executable content.**  
   After an assignment and before a send, the saved PC identifies the send. If suspension occurs while building the send payload, its builder and evaluated operands are retained. Completed work is not repeated. Live foreach frames retain their snapshot tokens.

10. **Suspension during eventless processing with observable `_event`.**  
    The actor’s current binding owns the event independently of its queue node and quantum. Eventless conditions and onentry code see that same binding after every resume.

11. **An internal event replaces `_event`.**  
    After the relevant eventless work, dequeueing the next internal event replaces the current binding. Later eventless transitions observe the internal event. The previous event is released when no other owner retains it.

12. **100,000 mostly idle actors with few active workspaces.**  
    Each idle actor retains its small persistent instance and directory slot. Empty queues allocate no nodes; stable actors allocate no workspace. A few active actors use pooled workspace/continuation storage. If many actors become suspended simultaneously, continuation demand grows accordingly. Current-event payloads, history, and datamodel contexts remain additional persistent costs.

For an illustrative compact chart, assume:

```text
32 bytes actor directory
32 bytes instance header
8 bytes configuration
8 bytes other persistent markers
```

Then 100,000 actors consume about **8 MB** before alignment changes, histories, datamodels, retained events, and shared pools. That is a sizing example, not a benchmark claim. A Python context per actor could dominate it.

**24. What to optimize—and what to prove**

The common path should be:

```text
event-name handle
→ cached descriptor matches
→ ordered candidate transitions
→ datamodel condition tests
→ interval/bitset conflict and exit operations
→ structural action bytecode
→ ordered configuration updates
```

Optimize the common cases directly:

- one active leaf;
- one selected transition;
- no history;
- constant transition domain;
- literal local send;
- no executable content;
- small configuration fitting one word.

Keep a correct general path for parallel regions, history, dynamic expressions, multiple targets, and suspension.

Do not add a general object framework, universal value system, native-fiber scheduler, transport bus, or heap continuation hierarchy.

Validation needs three complementary approaches:

- conformance coverage using the [W3C implementation test suite](https://www.w3.org/Voice/2013/scxml-irp/);
- differential semantic traces against a simple reference implementation;
- systematic forced suspension before and after every resumable operation.

The forced-suspension tests are particularly valuable: compare an actor’s trace under large fuel and under tiny fuel, controlling external delivery order. Include `_event`, action errors, history capture, finalize, duplicate send IDs, timer ties, and cancellation.

Benchmark kernel execution separately from datamodel work and local messaging. Measure bytes per idle actor, bytes per suspended actor, retained-event cost, chart image size, transition throughput, and worst work before returning to the host.

The resulting library can be a tiny portable actor-system VM. Its compactness comes from immutable shared charts, chart-dependent persistent state, pooled messages, and explicit continuations. Its correctness comes from keeping semantic event lifetimes, ordering, routing, and invocation inside the library—and making resource and datamodel limits explicit.
