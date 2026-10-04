# Building blocks: what core has, what the proposal needs, what can go

**Status:** Sketch (2026-10-03), part of [the ring protocol proposal](../README.md)

The proposal describes a set of structures: three rings, an envelope pool,
a name area, a park area, a deadline index, an internal queue, and slot
records. Under the specific names there are only about a dozen kinds of
structure. This document lists those kinds as **building blocks**. For each
block it says what core has today, what the proposal needs, and how each
structure in the proposal is built from blocks. If every block exists
first, has its own contract and tests, then implementing the proposal is
mostly a matter of wiring the blocks together.

Line references are to `tinyactors-core/src` unless a path says otherwise.
They were taken on 2026-10-03.

## Lifetimes come first

A block's contract is mostly about **when its memory can grow and when its
contents die**. The proposal uses these lifetimes. Every block belongs to
exactly one of them.

| Lifetime | Begins / ends | May allocate? | What lives here |
|---|---|---|---|
| **system** | create / destroy | at creation; grows only at safe points, up to a bound | control block, rings, pool pages, name and park chunks, running workspace, atom tables |
| **definition** | register / free | at registration | compiled chart, authored atoms, mandatory-event bounds |
| **session** | spawn or load / unload or destroy | at spawn or load | actor record, invocation slots, list heads |
| **envelope** | admit or send / free | never (taken from the pool) | envelope, its text chain, its vref |
| **input** | take / `c2h.input_completed` | never | slot record, outgoing list, c2h reservation, chain count |
| **parked** | park / resume | never (chunks from the free list) | serialized continuation and pending internal events |
| **macrostep, unit** | start / end | never (reset arenas) | scratch, render region, resolved event atoms |
| **record** | commit / release | never (ring bytes) | a record and the spans in its tail |
| **call** | call / return | never | params, faults |

**Safe point**: a moment when no slot is inside a unit of executable
content. That is during admission, after a macrostep ends, or at
registration, spawn or load. "A macrostep never allocates" means that only
safe points may refill a block.

## The catalog

Verdicts:

- **have**: exists and is fit to use;
- **generalize**: exists, but is tied to one subsystem or written more than once;
- **new**: does not exist;
- **nice**: not required, but makes everything easier.

### A. Memory

| # | Block | Contract | Today | Proposal uses it for | Verdict |
|---|---|---|---|---|---|
| A1 | **Allocator** | A source of bytes and a sized give-back (`alloc`, `free(size, align)`). | `ta_allocator` (`include/tinyactors.h:228`); `ta_mem_*` wrappers (`types.c`); wasm size classes (`ta_allocator_wasm.c`) | everything allocated at system, definition or session lifetime | **have** |
| A2 | **Layout builder** | Adds `(size, align, count)` entries with checked arithmetic, returns offsets and a total. Turns N regions into one allocation. | Hand-written six times: `ta_actor_layout` (`actor.c:33`), continuation offsets (`continuation.c:13`), `ta_need_take` (`interpreter.c:4015`), `ta_event_block_bytes` (`interpreter.c:707`), operator jump, system region (`system.c:101`) | control block + three rings; the running workspace with its mandatory arena (the proposal requires checked arithmetic before publishing); actor record with list heads | **generalize** |
| A3 | **Bump arena** | Grows by chained blocks, or is fixed over caller memory. Reset is all or nothing. | `ta_arena` (`arena.c`, 173 lines), `ta_arena_init_fixed` | definition compile; scratch and render regions | **have** |
| A4 | **Arena mark / rewind** | `mark()` returns a position; `rewind(mark)` frees everything allocated after it. | missing; only `reset` | scratch for one unit inside a macrostep, rewound at unit end, without resetting the whole scratch region; the name resolver's atom buffer | **nice** |
| A5 | **Index pool** (slab with safe-point refill) | Fixed-size items addressed by `u32` (0 = none). Items live in pages that never move. Each item carries an intrusive free list. `take` and `give` are O(1) and never allocate. `refill(min_free)` allocates pages up to a ceiling, and may be called only at a safe point. Counters: `used`, `free`, `high_water`. An optional `trim` gives pages back. | The "fixed pages plus a `ta_vec` directory of pages" pattern is written **five** times: actor table (`actor.c:14`), timer owner heap pages (`timer.c:26`), address buckets (`address.c:11`), session buckets (`session.c:14`), io_origin pages (`io_origin.c:5`). Free lists are hand-rolled for actor slots, definition slots, io_origin and the event-block cache. | **envelope pool** (88 bytes per item); **name-area chunks** (64 bytes); **park-area chunks** (256 bytes); **slot records** (up to `maxConcurrentInputs`); actor table | **generalize**. This is the most important block: the proposal needs four instances of it, and core already has five copies of its pattern. |
| A6 | **Chunk chain** | A byte stream stored in chunks from an index pool. Each chunk holds a payload and a `u32` link to the next. `need(n)` gives the chunks n bytes will take, for preflight. `write` appends; `read` iterates spans; `free` returns the whole chain. A chain is owned by exactly one owner: an envelope or a parked slot. | none | **name area**: the length-prefixed text fields of an envelope; **park area**: a parked continuation and its pending internal events | **new**. One block, parametrized by chunk size, gives both areas. |

### B. Containers over indexes

| # | Block | Contract | Today | Proposal uses it for | Verdict |
|---|---|---|---|---|---|
| B1 | **Intrusive index list** | A `{head, tail}` of `u32` indices, linked through a field of the item that the caller chooses (`next` or `owner_next`). Operations: `push_back`, `push_front`, `pop_front`, `splice` (append a whole list in O(1)), and `unlink(i)` (walks the list; bounded by the pool). Uses no memory of its own. | Only in the kernel: mailboxes (`tinyactors-host/src/scheduler_private.h:9`). Core uses intrusive pointers in the event-block cache. | **run queue**, **deferred list**, **waiting list**, **outgoing list** (spliced onto the run queue at the macrostep end), **owner list** (`<cancel>`), the pool's **free list** | **new** in core. One block covers all six lists. |
| B2 | **Indexed min-heap** | A heap of `u32` items keyed by `(due, order)`. It stores each item's heap position back in the item (`heap_index`), so `remove(i)` costs O(log n). Its storage grows with the pool, and only at safe points. | `ta_timer_heap` (`timer.c`, 492 lines) is a heap of per-owner sorted lists and is tied to delayed sends. The kernel's wake heap (`loads.c:416`) and the compiler's heapsort (`builder.c:1060`) are two more heaps. | **deadline index** over envelopes; `c2h.wake` reads its head | **generalize**. Replaces the timer heap's owner-list design. |
| B3 | **Hash table, grows only when told** | Open addressing, keys hashed by the caller, `u32` values, delete by backward shift (no tombstones). Two modes: **fixed** (capacity set once, so insert never allocates) and **growing**, where growth happens only in an explicit call at a safe point. | **Four** implementations: atoms and event names (chained, with near-identical rehash code: `atom.c:13` and `event_name.c:13`); addresses and sessions (paged linear hashing, near copies of each other). The kernel adds `tah_map` and `tah_id_set`. | atom lookup (`find`, no insert, inside macrosteps); **waiting lists keyed by address bytes** (fixed mode, bounded by `poolEnvelopes`); the open `source_seq` index for `h2c.withdraw`; resident session and address handles | **generalize**. One table can replace four. |
| B4 | **Fixed-entry FIFO** | A circular array of fixed-size entries. Ordered `push` and `pop`, `peek` at the head (the taken entry stays at the head while it is `_event`), and `clear`. | The event block's queue array, whose growth code is written twice (`interpreter.c:755` and `interpreter.c:4445`) | the **internal queue**: one live queue in the running workspace and one per nested interpreter | **generalize**. Becomes fixed-size; quota accounting comes from C3. |
| B5 | **Bitset** | Word-array bits. | `ta_bits_*` (`types.h:52`) | configuration, history and the interpreter's sets; parked as bytes | **have** |
| B7 | **Type registry** | A set of declared type names (atoms) with built-in entries that core owns and their aliases (`scxml`, and SCXML's URI forms). `declare(name)` happens outside a macrostep and allocates; `declared(name)` is a lookup that never inserts. No behaviour is attached to an entry. Withdrawal is not supported yet; adding it later only refuses new uses. | Two registries, each with a vtable per entry: I/O processors (`system.c:690-752`, with registry indices) and invokers (`system.c:753-785`) | `<send type>` and `<invoke type>` checks, which raise `error.execution` in the chart; declared by [`h2c.type_declare`](../protocol/generated/MESSAGES.md#h2ctype_declare) | **generalize**: one block over B3, created twice (I/O processors and invokers) |
| B6 | **Growable array** | Doubles, copies, holds indices rather than pointers. | `ta_vec` (`types.c:48`), plus about five hand-rolled parallel-array copies (invokes, timer lists, export blocks, arena sink) | compiler only. Nothing in a macrostep may use it. | **have**; fold the copies into it |

### C. Transport and accounting

| # | Block | Contract | Today | Proposal uses it for | Verdict |
|---|---|---|---|---|---|
| C1 | **SPSC byte ring** | One producer and one consumer on one thread. Producer: `reserve(n)` (aligned to 8; writes a pad at the wrap) → `fill` → `commit`. Consumer: `peek` → `release`. `head` and `tail` are `u64` counters that only grow. Assigns a gap-free `seq`. Two modes: **lossless**, where full means refuse, and **lossy**, where full means drop the record, count it and still consume its seq. | None in core. The kernel's `tah_ring` is a chunked FIFO of items, which is a different structure. | **h2c**, **c2h** (lossless), **tel** (lossy), plus their control block | **new**. One block with three instances. |
| C2 | **Record codec** (generated) | For each message, generated from `protocol/catalog.rb`: a C struct (already generated: `ring_proto.h`); `size_of(text lengths)`; a writer that fills in place into a reservation; and a **validating reader** that checks `size ≥ fixed`, that every span lies inside the record, that the kind is negotiated, and that seq has no gap. Also a JSON decoder for the test host (`messages.json`). | The structs and the JSON exist. There are no writers or validators. The only framing code in the repo is the kernel's OTLP varints. | about 40 messages. h2c validation is where hostile input enters, so it is the security boundary. | **new**, and it must be **generated**. Writing 40 writers by hand is where bugs will come from. |
| C3 | **Quota with reservations** | `{capacity, used, reserved}`. `reserve(n)` succeeds only if `capacity - used - reserved ≥ n`; `consume(n)` draws from a reservation; `cancel(n)` returns it. A holder (a slot, a phase) records how much it reserved, so a parked slot keeps its claim. | Scattered as ad-hoc checks: `ta_unit_reserve` (`interpreter.c:2430`), `ta_actor_reserve_send` (`actor.c:102`), `ta_timer_reserve*`, `ta_interpreter_queue_room` | the **c2h mandatory reserve** (bytes, including the worst-case pad); the **pool reserve** for one macrostep; the **admission watermark**; the internal queue's **ordinary quota** and **mandatory quota**; the park area's reserve | **new**. It is tiny, but it gives "mandatory records never fail" one place to be proved. |
| C4 | **Claim set** (reserve all, then commit) | Reserves from several quotas at once (an envelope, name chunks, record bytes). Either every reservation succeeds or none is kept. Commit or abort is O(1). | The idea exists as the two-part `<send>` (`interpreter.c:2122`) and the three-part invoke start (`invoke.c:575`), written separately for out-of-memory safety | `<send>`, delayed send, local invoke token, admission (envelope + chunks + checking the `c2h.input` size), recycling | **nice**. It turns every "fails as `error.execution.capacity.<resource>`" into one code path that names the resource that ran out. |
| C5 | **Sequence counter** | Monotonic `u64`, starting at 1. The h2c gap check lives here. | implicit | ring `seq`, `invocation_seq`, `request_seq`, macrostep numbers | part of C1 |

### D. Bytes, text and names

| # | Block | Contract | Today | Proposal uses it for | Verdict |
|---|---|---|---|---|---|
| D1 | **Byte sink / source** | `begin/write/end` and `read`, provided by the binding (ADR 0090). | `ta_byte_sink`, `ta_byte_source` (`include/tinyactors.h:386`). The only implementations are `ta_arena_sink` and `ta_memory_source`, private to `session_image.c:1015`. | images into host blobs (`c2h.unloaded`, `c2h.state`); `h2c.load`; definition specs; **a sink over a chunk chain** for parking | **generalize**. Move the two implementations out of `session_image.c` and add a sink and a source over a chunk chain. |
| D2 | **Binary serializer** | A typed writer and reader over D1 (u32, u64, bytes, bitsets, values through the data model). Its format is private to the build, checked by a magic number and a version. | `ta_offload_put` / `ta_offload_take*` (`session_image.c:997` onward), which `memcpy` structs into one buffer | session images, checkpoints, and **parking** (the same continuation, written to a chunk chain instead of a blob) | **generalize**. One serializer gives images, checkpoints and parking. |
| D3 | **Text writer** (measure, then fill) | Bounded, counts the full length, has a skip window. `ta_text_render` runs a writer once to measure and once to fill. | `ta_text_writer` (`types.h:71`) | rendering `#_send.<n>`, `<stateid>.<platformid>`, `done.invoke.<id>` and `error.communication.invoke.<id>.<reason>`. Measuring first gives the exact size to reserve in a ring or chunk chain; then the writer fills it. | **have**. Add targets: a chunk chain and a record's tail. |
| D4 | **Name resolver** | Splits name bytes at the dots, calls `ta_atom_find` on each segment, and writes up to `eventNameDepth` atoms into scratch. A segment that is not an atom becomes UNKNOWN. Never inserts. | `ta_atom_find` (`atom.h:27`) and `ta_pattern_match_run` (`definition.c:66`) exist; the splitter does not | resolving a taken envelope's name; `typeexpr` and `<cancel sendidexpr>` lookups | **new**, and small |
| D7 | **Trie of atoms** | Interns a path of atoms as nodes keyed by (parent, segment atom) and gives each path one stable ID. Inserts only at safe points; lookup never inserts. | `ta_event_name_table` (`event_name.c`, 204 lines; ADR 0007). Today it is tied to event names, and its hash table is a near copy of the atom table's. | authored event descriptors at registration. It is **kept as a block**: anything named by a dotted path of authored segments can use it (descriptors, family addresses, I/O processor types, and later uses). Run-time names no longer enter it (D4). | **generalize**: name it as a block, put it on B3, and keep event names as one user of it |
| D5 | **Hash functions** | One header: FNV-1a over bytes, and a fixed 64-bit mixer. | The Murmur finalizer is copied verbatim (`session.c:4` and `trace.c:34`); FNV-1a exists in 32-bit (`atom.c:3`) and 64-bit (`builder.c:350`) forms; splitmix64 appears in core and in the kernel | B3, span IDs, definition fingerprints | **generalize** |
| D6 | **Small formatters** | Decimal, hex, percent-encoding. | Decimal is written three times, hex four times, `cstr` to `ta_str` twice | D3, `scxml_uri`, trace IDs | **generalize** |

### E. Identity and test support

| # | Block | Contract | Today | Proposal uses it for | Verdict |
|---|---|---|---|---|---|
| E1 | **Generation handle** | `(generation << 32) | slot` with `make`, `slot`, `generation` and `valid`. | `ta_actor_id_make` (`actor.c:78`); definition IDs repeat the same packing by hand (`system.c:456`); `ta_io_scope` has its own scheme | sessions and definitions, as today. Envelopes do not need one, because withdrawal goes by `source_seq`. | **generalize** |
| E2 | **Failing, counting test allocator** | Refuses from the Nth allocation; counts bytes per category. | Hand-rolled in nine core test files (`params_test.c:36`, `deep_test.c:34`, `span_test.c:43`, ...); the kernel has `tah_allocator_refuse_from` | the shrunken out-of-memory sweep: creation, registration, spawn, load, refill | **generalize** into one core test helper |
| E3 | **Ring script driver** | A C test helper. It posts h2c records (using the C2 writers), calls `ta_run`, and checks the c2h sequence against an expectation. | none (the test host's JSON-RPC protocol does the same job over a socket) | core tests for scenarios S01 to S11, without a binding | **nice** |

## Assembly: each proposal structure from blocks

If the blocks above exist, every structure in the proposal is a
composition of them, plus the policy that belongs to it alone.

| Structure | Blocks | Its own policy (what is left to write) |
|---|---|---|
| `h2c`, `c2h`, `tel` + control block | A2 (one allocation), C1 × 3, C2, C3 (c2h reserve) | which ring is lossy; the `admitting` flag |
| Envelope pool | A5 (88-byte items), B1 (free list), C3 (watermark, macrostep reserve) | the envelope's fields and states |
| Run queue, deferred, waiting, outgoing, owner lists | B1 × 5 over the pool | fairness (`fairnessRatio`); "move the first deferred envelope to the head of the run queue on completion" |
| Waiting lists by address | B3 (fixed mode) → B1 head | `c2h.load_needed` once per address; `request_seq` checks |
| Deadline index | B2 over envelope indices | `c2h.wake` when the head changes; due I/O sends become `c2h.send` |
| Name area | A5 (64-byte chunks) + A6 + C3 | the field layout inside an envelope's chain (each field prefixed by a `u16` length); reserving room for a later `error.communication` |
| Event matching | D4 + the existing matcher | none |
| Internal queue | B4 + C3 × 2 (ordinary and mandatory quotas) | compiler sizing of the mandatory quota (still open) |
| Macrostep slots | A5 (slot records) + C3 (per-slot c2h claim) | turn-taking by `slotSlice`; a slot that waits for c2h; the chain limit |
| Park area | A5 (256-byte chunks) + A6 + D1 (chunk-chain sink) + D2 | "a slot that cannot park keeps the thread"; the derived ceiling |
| Running workspace | A2 + A3 (fixed) + B4 | grows at registration for the largest definition |
| Unload / load / checkpoint | D1 (host blob) + D2 | atomic load staging |
| `<send>`, delayed send, invoke token, admission | C4 over A5 + A6 + C3 | mapping the resource that ran out to `error.execution.capacity.<resource>` or `c2h.bounced` |
| Run-time names (`#_send.<n>` and the others) | D3 → A6 or a record tail | none |
| `ta_run` | all of the above | the loop: admit, fill slots, run, drain phases, return status |

The **index pool** (A5), the **index list** (B1) and the **quota** (C3)
appear in almost every row. Build them first.

## What core can remove

Removed as part of adopting the proposal. These sizes are estimates from
reading the code, not exact counts.

| What | Where | About | Replaced by |
|---|---|---|---|
| Per-actor event blocks and their cache (ADRs 0064, 0069, 0093) | `actor.h:45-128`, `interpreter.c:699-1000` and `:4441-4467`, `system.h:111-117`, `TA_PARAM_EVENT_BLOCK_CACHE`, readers in `actor.c` and `session_image.c` | 500 lines | A5 + B4 |
| Stopping where it stands on out-of-memory (ADRs 0047, 0070) | NOMEM branch in the run loop (`interpreter.c:4144`), the NOMEM parts of `ta_reserve_stop` / `ta_stop*` (`:3845-4013`), spare continuation and spare sparse state (`system.h:120`), `TA_OP_NOMEM` | 300 lines, plus public API | C3 + C4. **Keep** the resumable phases (`interpreter.c:3565-3845`) and `ta_continuation`: pause and parking still need them. |
| Queue mark / truncate rollback | `interpreter.c:843` and nine call sites | 20 lines, plus the call sites | Not needed once a raise can no longer be half-applied for lack of memory. Check each call site. |
| Delayed sends kept per owner | `timer.c` / `timer.h` (634 lines): owner lists, orphans, the heap's page directory, sparse state | 600 lines | B2 + B1 (owner list) over envelopes. `ta_send_id` stays. |
| Vtables that become records | router, I/O processor (`ta_io_vtable`, `ta_io_text_params`), invoker, directory, trace sink, log sink; the registration APIs `ta_system_register_io`, `ta_system_find_io`, `ta_system_register_invoker`; the I/O registry index; `TA_DM_TEXT_IO_TYPE` and `TA_DM_TEXT_IO_LOCATION` | 400 lines + header | C2 writers to c2h and tel. The two registries become B7, filled by `h2c.type_declare`: an unsupported `<send type>` or `<invoke type>` still raises `error.execution` in the chart. The I/O `text` callback and its templates go: see [I/O processors](#io-processors-and-invokers-one-type-registry-twice). |
| `io_origin` storage (ADR 0068) | `io_origin.c` / `io_origin.h` (262), `io.c` (206), public `ta_io_origin_make`, `ta_io_scope_open` / `_close` | 470 lines + API | Origin text in the envelope's name area. COMPUTED and SCOPED go: see [I/O origins](#io-origins-adr-0068-one-class-remains). |
| Run-time interning | `interpreter.c:1819`, `:1838-1856`, `:1354-1370`; `invoke.c:57-80`, `:950-975`; `scxml_uri.c:145`, `:303`; public `ta_send_id_intern`, `ta_system_intern_invoke_id`, the run-time use of `ta_event_name_intern` | 150 lines + API | D3 + D4 |
| Held effects for replay | `ta_activation_effects` (`actor.h:14-43`), `actor.c:124-200`, the replay-only send paths in `interpreter.c` | 300 lines | A replaying session writes no effects |
| The old drive API | `ta_actor_deliver` (with `TA_ERR_BUSY`), `ta_actor_resume`, `ta_system_dispatch_due` | — | `ta_run` |
| Dead code, removable now | `fault.h` declares two functions that nothing defines; the stale "fills at creation" comment (`interpreter.h:143`); a doc comment with no prototype (`actor.h:287`); unused `ta_configuration` kinds (only `WORDS` is ever chosen) | small | — |

The kernel's own containers (`tah_vec`, `tah_ring`, `tah_map`, `tah_free_list`,
`tah_id_set`, the counting allocator) go with `tinyactors-host`. Whatever napi
still needs comes from core's blocks or stays in napi
([migration.md](migration.md)).

**What stays unchanged:** the allocator, the arena, `ta_vec`, bitsets, the
text writer, the atom table and the trie of atoms (lookup inside
macrosteps, insert only at registration), the address and session tables (resident handles, ADRs 0065
and 0066), the actor table, the continuation, byte sinks and sources, the
data model and value vtables, the tree builder, and the pattern matcher.

### I/O origins (ADR 0068): one class remains

`io_origin` stores the URI that `_event.origin` shows for an event that
came in through an I/O processor. With that URI, a chart can reply with
`<send targetexpr="_event.origin" typeexpr="_event.origintype">`.
ADR 0068 gives the URI three storage classes, and under the ring protocol
only one of them remains:

| Class | Today | Under the ring protocol |
|---|---|---|
| MESSAGE | a refcounted copy of the URI, one per request | **stays, as text**: `origin` and `origin_type` in `h2c.event`, stored in the envelope's name area |
| COMPUTED | a `u64` token that the processor's `text` callback renders on demand | **goes**: the host renders the URI when it posts the event |
| SCOPED | one URI per connection; after `ta_io_scope_close`, the origin renders empty | **goes**: the processor's own map of live targets decides whether a reply is still deliverable, and drops a late one ([S12](../scenarios/S12-http-request.md#what-did-not-happen)) |

So `io_origin.c`, the scope API and the origin classes are deleted. The
reply target is text that the processor owns. The text costs name-area
bytes per event, which are bounded and accounted for. If the input
completes and the chart has not answered, the processor learns it from
[`c2h.input_completed`](../protocol/generated/MESSAGES.md#c2hinput_completed)
and its `ignored` flag.

### I/O processors and invokers: one type registry, twice

The I/O vtable (`ta_io_vtable`) has two callbacks, and core calls both
inside a macrostep:

- **`send`** delivers a `<send type="x">`. It becomes `c2h.send`, which
  carries `io_type` as text. The kernel's adapter already does no more
  than this: it copies each send into an effects queue that JavaScript
  reads later (`tinyactors-host/src/adapter_io.c`).
- **`text`** renders `_ioprocessors.<type>.location` and COMPUTED origins.
  Today this is a round trip. The binding's data model asks core
  (`TA_DM_TEXT_IO_LOCATION`, `TA_DM_TEXT_EVENT_ORIGIN`); core asks the
  binding's `text` callback; the callback expands a template the binding
  registered (`adapter_io.c:127`).

**Neither the callback nor the templates are needed under the ring
protocol:**

- **Location.** The data model belongs to the binding. It already gets
  the session's ID and name from core (`TA_DM_TEXT_SESSION_ID`, `_NAME`),
  and the binding knows its own processors. So the binding builds
  `_ioprocessors` itself and does not go through core. In napi,
  `location` becomes a pure function on the processor,
  `location({id, address})`, the inverse of its inbound route
  ([S13](../scenarios/S13-webhook-callback.md)). Core still
  provides the SCXML processor's own location, `#_scxml_<session>`.
- **COMPUTED origins.** The host renders the URI when it posts the event,
  and it travels as `origin` text in `h2c.event` and in the envelope's
  name area. That costs text per event, but the cost is bounded and the
  name area accounts for it. The COMPUTED class goes.

**What core still needs is a set of type atoms.** SCXML says that a
`<send>` whose type the platform does not support places
`error.execution` on the internal queue. That error belongs to the
sender's own block and macrostep, so the host cannot answer it later
from c2h. Core must therefore know the supported types when the `<send>`
executes. The registry shrinks from (type, adapter, index) to a set of
atoms, with no vtable, no templates and no registry index.

Registering napi's `console` processor:

1. `system.registerIO('console', processor)` keeps `processor` in
   JavaScript's `#io` map, as today, and posts
   [`h2c.type_declare`](../protocol/generated/MESSAGES.md#h2ctype_declare)
   `{registry: io, type: "console"}`.
2. A chart's `<send type="console">` checks the set (an unknown type is
   `error.execution`), then writes `c2h.send` with `io_type = "console"`.
3. The pump reads the record and calls `#io.get(io_type).send(request,
   context)`.
4. The data model lists `console` in `_ioprocessors` with whatever
   location the binding renders.

**Declaring is an h2c control, not a native call.** Whether a
`typeexpr` send succeeds or raises `error.execution` depends on which
types were declared when the input was taken. An ordered control puts
the declaration at a defined point among the inputs, so replay sees it at
the same point. A native call between `ta_run` calls has no defined
position relative to records already posted. Like
`h2c.definition_register`, the control takes no envelope. Declaring a
declared type does nothing, a name core owns is bounced as
`reserved_type`, and a type cannot be withdrawn yet.

**Invokers get the same registry, not the same operations.** The two
differ in everything except the type check:

| | I/O processor | Invoker |
|---|---|---|
| Shape | one-off messages, no lifetime | a service instance that lives as long as its invoking state |
| Core → host | `c2h.send` | `c2h.invoke`, `c2h.invoke_cancel`, `c2h.invoke_forward` |
| Host → core | `h2c.event` with `origin` and `origin_type` | service events and `h2c.invoke_report`, correlated by `invocation_seq` |
| State in core | none | an invocation slot, a pool token for a local child, an unload restriction |
| Listed in `_ioprocessors` | yes | no |
| Built-in type core owns | `scxml`: local mail | `scxml`: local child sessions |
| Undeclared type | `error.execution` in the sender's block | `error.execution` when the invocation starts, before the next external input (as `invoke.c:602` does today) |

Their operations already have their own records. What they share is the
registry, so it is one block (B7), created twice and filled by one
control with a `registry` field. Both vtables and both registration APIs
leave core. napi's `registerIO` and `registerInvoker` keep their
implementations in JavaScript and post the same record.

This refines ADR 0068, and ADR 0016 the same way: each registry becomes a set of types, `text` and
the COMPUTED class go, and MESSAGE becomes envelope text. SCOPED goes as
well ([above](#io-origins-adr-0068-one-class-remains)).
The log sink and the trace sink follow the same split. Their calls become
`c2h.log` and `tel` records, and their configuration (trace kind masks,
sampling) stays as data in core.

## A suggested order

1. **Collapse the duplicates.** This needs no ADR and changes no behaviour:
   - D5 and D6;
   - E1;
   - E2;
   - A2;
   - A5, adopted by the five paged tables;
   - B3, adopted by the four hash tables;
   - fold the hand-rolled arrays into `ta_vec`.

   Each is a refactor that `mise run test` covers, and each makes core
   smaller before the proposal adds anything.
2. **Add the new blocks, with tests of their own:** B1, B2, B4, C1, C3, A6,
   D4, and the C2 generator (in `mise/tasks`, emitting C from
   `catalog.rb`). None of them needs the interpreter.
3. **Assemble** the structures in the order of the table above: rings,
   then the pool and its lists, the name area, the deadline index, the
   internal queue, slots and parking, and finally `ta_run`. Delete each
   replaced subsystem in the same commit that replaces it.

## Open questions that affect blocks

- **Pages or one array for the pool?** The proposal says "the pool is an
  array" but also that it fills on demand. Pages (A5) give both: indices
  are stable and nothing is copied when the pool grows. One array would
  mean `realloc`, which moves envelopes. Pages are recommended.
- **Does B2 need pages too?** Its storage needs one `u32` per envelope, so
  it can grow in the same refill as the pool.
- **The c2h reservation with pads.** Before C3 can say a reservation in
  bytes is enough, it needs a worst-case pad term, such as one pad per
  record of up to `maxRecordBytes - 8` bytes. Whether a tighter bound is
  possible depends on C1's position at the time of the reservation.
- **A `trim` for A5.** The macrostep reference leaves open whether a call
  should return free pages above the reserve. If A5 is generic, adding it
  once covers the pool, the name area and the park area together.
