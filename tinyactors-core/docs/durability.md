# Durable state of a session

One definition of what a session *is* when it is not running, and of what
every path that takes it out of a running system and back keeps of it:
hibernation, export, migration and snapshots. None of them is a durability
mechanism: hibernation and snapshots take actors out of memory for a while
(and snapshots let a host be inspected), export moves actors to another host.
Durability is the application's (a database, for example).
A path that drops part of it changes what the session does next;
`tests/unit/test_durable.c` checks that none does.

## The inventory

A session's durable state is everything that can influence what it observably
does later: its logs and effects, its configuration, its datamodel, the ids it
generates, and when its delayed sends fire.

| # | Part | Lives in |
|---|------|----------|
| 1 | session id (`_sessionid`, `#_scxml_<id>`) | actor record `serial` |
| 2 | name (`_name`) | actor record `name` |
| 3 | chart (and the image it belongs to) | instance header `chart`; image content hash |
| 4 | configuration | instance: `config` bitset |
| 5 | history recordings | instance: `history_recorded` flags + `history` bitsets |
| 6 | late binding: which states' `<data>` is initialized | instance: `data_init` bitset |
| 7 | invocations to start at the end of the macrostep | instance: `to_invoke` bitset |
| 8 | generated-id counter (`idlocation` sendids, generated invokeids) | instance header `ids` |
| 9 | datamodel values (`<data>`, variables scripts created) | datamodel instance |
| 10 | the bound `_event` (name, type, sendid, origin, origintype, invokeid, data) | datamodel instance (the core's pinned event record is only a cache of it) |
| 11 | `<invoke>` params of the session (late binding applies them to `<data>` of states entered later) | datamodel instance (`init`) |
| 12 | running SCXML invocations: the child sessions, each with all of this, its slot and invoke id | children's records + parent's `invoke_children` / `invoke_ids` |
| 13 | pending delayed sends: due time, firing order, route and target, event name, sendid, type, payload | timer pool + heap; events; datamodel payloads |
| 14 | queued external events (mailbox) | event pool |
| 15 | queued internal events | instance `iq_*` (empty at every stable point, except a parked `error.communication`) |
| 16 | the continuation inside a macrostep: phase, sub-phase, cursors, workspace (PC, `<foreach>` frames and items) | instance header, workspace pool, datamodel iterators |
| 17 | pending requests (dynamic `<invoke srcexpr>`, spawns parked by pool exhaustion), external invocations | request pool |
| 18 | a requested termination / cancellation | actor flags, instance `INSTANCE_FLAG_CANCEL` |
| 19 | step counters: macrosteps and microsteps (they name the session's spans, ADR 0006) | instance header `macrostep`, `microstep` |
| 20 | trace contexts of waiting events: the trace, its flags and the step that caused each (delayed sends, mailbox) | context pool, referenced by events |

Not session state, by design:

- **Ids of sessions created later** are allocated by the system that creates
  them (`first_serial` ranges, `ta_system_set_next_serial`): a child invoked
  after a move gets an id from its new system.
- **Actor handles** are re-issued by every system; a session's address is its
  session id.
- **Order between independent sessions**: timers of different session trees
  due at the same tick fire in adoption order after a move, and a hibernated
  session runs when the host wakes it. SCXML defines no order between sessions;
  each session's own event order is unchanged.
- **Scheduling and accounting**: run-queue position (sessions are idle at the
  points where they are hibernated or exported), the CLOCK bit, statistics,
  `ta_actor_info.current_event_name` after a hibernation or a move (inspection
  only; `_event` itself is #10).

## What each path keeps

✓ = kept · — = not applicable · ✗ = refused (`TA_ERROR_STATE` /
`TA_ERROR_UNSUPPORTED`): the path does not run, the session stays as it was.

| # | hibernate → wake | export → adopt (inside a migration) | + `ta_blob_migrate` | system / host snapshot |
|---|---|---|---|---|
| 1, 2 | ✓ record stays | ✓ in the blob | ✓ | ✓ region |
| 3 | ✓ same chart and image | ✓ same image hash | translated to the new chart by state id | ✓ same image hash |
| 4–7 | ✓ instance words | ✓ instance words | translated (below) | ✓ |
| 8 | ✓ | ✓ | ✓ | ✓ |
| 9–11 | ✓ datamodel `serialize` | ✓ datamodel `serialize` | ✓ + `migrate` hook | ✓ datamodel snapshot |
| 12 | ✗ a session with running invocations | ✓ the whole tree | ✗ a tree (moved unchanged; translated once its invocations finished) | ✓ |
| 13 | ✓ stays in the region (fires while hibernated → mailbox → `TA_EFFECT_WAKE`) | ✓ in the blob, payloads as values | ✓ | ✓ |
| 14 | ✓ stays (and keeps accepting events) | ✗ unless empty | ✗ | ✓ |
| 15 | ✗ unless empty; a parked `error.communication` moves in on wake | ✗ unless empty | ✗ | ✓ |
| 16 | ✗ not at a stable point | ✗ | ✗ | ✓ (mid-macrostep snapshots are valid) |
| 17 | ✗ | ✗ pending requests; ✓ external invocations, in the blob (id, slot, type, src, invoke id, params as a value) | ✓ kept when their state and `<invoke>` are, else stopped (`TA_EFFECT_UNINVOKE`) | ✓ |
| 18 | ✓ applied on wake | ✗ | ✗ | ✓ |
| 19 | ✓ instance header | ✓ instance header | ✓ copied with the header | ✓ |
| 20 | ✓ stays in the region | ✓ in the blob, as values (re-created on adoption; untraced if the adopting system's pool is full or its tracing is off) | ✓ | ✓ region |

Why the refusals:

- **Hibernation needs a stable point without children** (12, 15–17): invoked
  children read their parent's instance (invoke slots, `done.invoke`,
  finalize), and a continuation refers to region objects (workspace, pinned
  event records) that a blob cannot hold. Hibernate the children (leaves
  first) or export the tree.
- **Export needs every session idle with empty queues** (14–18): queued events
  and requests are in flight *to* the session; the host runs the system first
  (the next `ta_system_run` drains them) and exports then. An external
  invocation is no request in flight: it is a service the embedder runs,
  named by an id that does not change with the segment, so it leaves with
  its actor. Its id is unique in the host, not across a
  host and a copy restored from its snapshot, which go on giving the same
  ids: it is no idempotency key for a service outside the process.
- **Snapshots** are a copy of the whole host, every segment and chart version
  at once: for rehearsing a deploy on a copy, capturing a host for a
  developer, preparing tests, and taking a host out of the process for a
  while; never for durability or backups: a snapshot is raw region memory,
  valid for a build of the library that lays the region out the same way
  only (below, "Snapshots name their layout"). They are the region bytes
  plus the datamodel's own snapshot; they need a drained outbox
  (`ta_system_snapshot`) or a quiescent host (effects
  acknowledged, no migration: `ta_host_snapshot`). Hibernation blobs stay
  with the host and are not part of a snapshot. External invocations are
  kept with their type, src, invoke id and params, but the services the
  embedder runs are not in a snapshot: the restored host's first run
  announces each invocation again (`TA_EFFECT_INVOKE` with
  `TA_EFFECT_FLAG_RESTORED`), and the embedder starts a service (the real
  one, or a stand-in on a copy) or answers the invoker with
  `error.communication`. Params are kept when the datamodel can retain a
  payload (both shipped ones can). An invocation whose service said done or
  failed is not announced again.
- **Migration** changes the chart, so it is observable by design: the session
  processes `error.platform.upgrade` (its `_event` until the next event; its
  data the previous bindings) before its mailbox. States are mapped by id and
  the configuration completed to a legal one; history recordings survive only
  whole; late-binding flags follow their states; `<data>` the new chart adds
  is initialized.

### Datamodels

| Datamodel | `serialize` (hibernate, export) | payloads in export blobs | snapshot |
|---|---|---|---|
| null | nothing to keep (`_event` unobservable) | — (no payloads) | — |
| ECMAScript (C, quickjs-ng) | `JS_WriteObject` (object graphs, cycles, Date/Map/Set/typed arrays) of: shared-realm sessions `[2, variables + _event, invoke params]`; isolated sessions (scripts declaring globals) their global properties, global `let`/`const`/`class` bindings, top-level function declarations (re-run on restore) and invoke params. Functions in data and classes ✗ (`ta_datamodel_ecmascript_last_error` names the variable) | values (`payload_serialize`) | `ta_datamodel_ecmascript_snapshot`: sessions, invoke params, payloads, `<foreach>` items, at their handles |

Export and adoption are the steps of a migration inside one host
(`ta_host_migrate`): the host exports a session from its old segment,
translates the blob (`ta_blob_migrate`) and adopts it into the segment of
the current version, routing to it there. No host call hands an export
blob out.

## Blob formats

Blobs and snapshots are for one build of the library: instance words and
the region are copied verbatim. Each format carries a version, and a blob or
snapshot whose version differs from the build's is refused
(`TA_ERROR_INVALID`), never read: a blob wakes, and a snapshot restores, only
in a build of the same format. The versions are `HIBERNATION_VERSION` and
`EXPORT_VERSION` (`src/system/hibernate.c`) and `SNAPSHOT_VERSION` for a
host's snapshot (`src/host/host.c`); a change to a format changes its version.
A system's region carries its layout id instead (below).

### Snapshots name their layout

A region is the build's structs as the compiler laid them out, so a snapshot
can be read only by a build with the same layout. The system layer describes
that layout (`ta_system_layout_description`, `src/system/region_layout.c`): as
CBOR, the library's version, the byte order, the size of a pointer, `TA_NIL`,
and every type a region holds (`ta_system`, what it embeds, and the records
of its pools and arrays), each member with its name, offset, size, count,
type and, for a link, an offset or a string, its kind (`relative`,
`index:<pool>`, `handle:<pool>`, `pointer`, `string`, `event-name`, `slab`,
`global:<section>`), with a
`when` (a member of the record and the values of it) where the kind holds
only for some records (a request's `source`, `type` and `next`); then the
pools and the arrays `ta_system` points to, with the member that holds each
one's count; then `strings`, how a string member reads as text. Every
number in it comes from `offsetof`, `sizeof` or the constants the code
uses, and `mise run layout` checks the table against the debug
information: every member of every type is there, at the compiler's offset
and size.

A `string` member holds a string reference: `strings.none` (`TA_NIL`) for
none; with `strings.slabBit` set, a slab string of the region, whose offset
in the slab's pages is the rest shifted left by `strings.slabShift` (the
pages lie at `ta_system`'s member `strings.slab` plus that member's
`strings.pages`), where a `strings.header` (`ta_shared_string`) lies and its
`strings.length` bytes follow; otherwise a string id of the segment's image
(a pack holds segment *i*'s image as its image *i*): the offset, in the
image's strings section, of a u32 length and the bytes. An `event-name`
member is an index of the image's event names section, a record whose
`name` is such a string id. `strings.image` says how to find the sections:
the image's integers are little-endian, its `header` (`ta_header_record`)
is followed by its `sectionCount` `section` records (`ta_section_record`),
each a `kind`, an `offset` from the image's start, a `size` and a `count`;
`strings.image.strings` and `strings.image.eventNames` are the kinds of the
two sections, `eventName` the type of an event name's record and `name` its
member. So a reader resolves an actor's name, an event's name, sendid,
origin, origin type, invoke id and raw message, a delayed send's target and
an effect's strings from the snapshot alone.

The description goes on past the region, so a reader decodes everything a
snapshot holds without the build:

- **`image`**: every record type of the image (`ta_chart_record`,
  `ta_state_record`, `ta_transition_record`, `ta_expression_record`,
  `ta_capture_record`, `ta_send_record`, `ta_invoke_record`,
  `ta_data_record`, `ta_actor_record`, with the header, section and event
  name records), described as the region's are, and how to find them:
  `sections` names each section kind and its element type, `chart` is a
  chart's record, and `ranges` says which members of it give the first and
  the count of the chart's records in each section. An image record's
  member kinds are `string`, `event-name`, `chart:<section>` (an index
  local to the chart's records of that section), `global:<section>` (an
  index of the whole section) and `u32-pool` (an offset, in u32s, into the
  `u32` section, where its `list` lies: `count` entries, each of the kinds
  in `elements`, as a transition's targets and its event descriptors, the
  event names lo <= e < hi). `names` gives the names of a member's values
  (a state's `kind`: atomic, compound, parallel, final, history-shallow,
  history-deep), and `flags` its bits (a transition's internal, history
  target and initial). So a chart decodes whole: its name, datamodel,
  source, states with their parents, children and initial states,
  transitions with their sources, targets, events and conditions, and
  data. The image holds no SCXML text: a chart's `source` names the
  document it was compiled from.
- **`instance`**: where an actor's active configuration lies. A resident
  actor's `instance` is a slab offset (its kind is `slab` while the actor's
  state is resident, `global:charts` while it is hibernated: the chart, its
  configuration being in its blob), where a `ta_instance` header lies, then
  the `areas` of u32 words of `ta_instance_layout_of`, in order, each as
  long as its `words` says (`stateWords`: one bit per state of the chart,
  rounded up to whole words, or a member of the chart's record) and present
  when its `when` holds for the chart's record. `config` is the active
  configuration, one bit per state in document order, least significant
  bit first; a history state's recorded states are bits of `history` from
  its `historyOffset`, over its parent's descendants. The layout of an
  instance depends only on the chart's record, so the description gives
  the rule, not a table per image, and its id stays the build's.
- **`trace`**: the trace records the region's journal keeps
  (`<tinyactors/trace.h>`): the journal's member of `ta_system` and the
  members that bound its records, the header type (`ta_trace_record`) and,
  per kind, its name, its body's type and the u32 lists after the body
  (a macrostep's configuration, a microstep's transitions, exited and
  entered states), each with the body member that counts it. A record's
  strings are string references, read as above.

The journal keeps the records a host already took as history: an
acknowledged record stays in the storage, between `head` and `read`, until a
new record needs its room, which drops the oldest first, and only the
records not yet taken (from `read` to `tail`) make a record be lost. So a
snapshot carries the most recent `traceBytes` of records, and a reader turns
them into the spans of the moments before the capture, as the OTLP encoder
names them. What it cannot have: a dynamic string of an acknowledged record
(a runtime actor's name, an event name the image does not hold) was released
when the record was taken and may have been reused, so a reader reads those
only for pending records, and the values a record names (with
`TA_TRACE_VALUES`) were released too. The host's encoder state (spans it
staged and has not handed out, open spans, its wall-clock marks) is host
memory, not a region's, and is not in a snapshot; the records it read are,
so nothing is missing but the wall times: a reader places records by the
system's clock and fuel stamps.

The FNV-1a 64 of those bytes is the **layout id**. `ta_system_init` writes it
at offset 8 of the region, after the magic, and `ta_system_restore` refuses a
snapshot whose id differs from the build's (`TA_ERROR_INVALID`): a change to
any region type, an inserted field included, changes the id, with no version
to bump by hand. A pack (`ta_host_snapshot_pack`, format 4) carries the
description itself and its id, so a reader that is not this build (the
website's inspector) can decode the regions without the build's headers, and
`tinyactors inspect` says whether a snapshot is of this build's layout.
`ta_host_snapshot_unpack` refuses a pack of an earlier format, and one whose
layout id is not the FNV-1a 64 of its layout part.

- **hibernation**: header (its writer's identity
  among its fields) | instance words | invoke ids as text | datamodel bytes.
- **export**: header | per session, parents first: header |
  instance words | invoke ids | datamodel bytes | delayed sends (absolute due,
  rank in the tree's firing order, route, slot + 1 for `#_<invokeid>`,
  target/name/sendid text, payload bytes or handle) | `<data>` to initialize
  (migrated) | name.

Both are read without a system by `ta_blob_read`, `ta_blob_session_at`,
`ta_blob_configuration` (with the image the blob was written with) and
`ta_blob_delayed_at` (`src/system/system_internal.h`): what a blob holds,
in place, allocating nothing and calling no datamodel. The `tinyactors`
command reads blobs with them, and snapshots by restoring them into a host
whose datamodels do nothing ([cli.md](cli.md#snapshots-and-blobs)).

## Fixed by this inventory

`tests/unit/test_durable.c` found these; each has a deterministic test there.

- **Generated ids were system state.** One counter per system made a
  session's ids depend on its neighbours and on the system it lived in (the
  export header carried the counter as a stopgap). It is now `ids` in the
  instance header (in former padding: the header stays 48 bytes), so every
  path keeps it with the instance; an instruction retried after pool
  exhaustion generates the same id again. Ids are unique within the session
  for 2³² generated ids.
- **Delayed-send payloads were handles in export blobs**: a blob adopted by
  another datamodel host (another process, a restart) delivered another value
  or none, and a dropped blob leaked the reference. Payloads are now values
  (`ta_datamodel_vtable.payload_serialize` / `payload_deserialize`).
- **Invoke params were lost on hibernate/export** (shared-realm sessions of
  the C ECMAScript datamodel, and the JS bindings' datamodel): a late-binding
  child initialized its `<data>` from the default expression instead of the
  param after a round trip.
- **Same-tick delayed sends of a tree changed order on adoption** (re-armed
  session by session): each now carries its rank in the tree's firing order.
- **An isolated session could not be stored after an upgrade, or broke**
  (ECMAScript datamodel, found once the test had a chart declaring globals):
  the migrate hook put top-level function declarations into
  `error.platform.upgrade`'s data — a payload that cannot be written, so the
  datamodel snapshot and the session's next hibernation/export failed — and
  deleted the new chart's own `let`/`const` names (only `<data>` ids were
  kept), so its scripts threw `error.execution`. Functions are code the new
  chart re-creates, not bindings; names the new chart declares stay.
- **A delayed send that failed at fire time** (its target gone) raised
  `error.communication` into an idle sender without scheduling it — the error
  waited for the next event, and the session could be neither hibernated nor
  exported meanwhile — and into a *hibernated* sender's nonexistent instance
  (memory corruption). The sender now runs at once, or gets the error parked
  in its mailbox and moved to its internal queue on wake.

## Trace state (ADR 0006)

What a session's trace needs is in rows 19 and 20. An instance's own context
(the current macrostep's) is a region reference and is let go at every stable
point, so no blob holds one. Records waiting in the journal are not session
state: they are the host's to read, and a snapshot keeps them with the region;
a hibernation or an export does not move them. The contexts of delayed sends
are in the export blob.

## Per-chart counts (ADR 0007)

The counts of each chart (`ta_chart_stats`) are system state, not session
state: they live in the region, so relocation and a system snapshot keep
them, and a hibernation or an export leaves them where they were made. A host
keeps the totals of the segments it retired, by chart name, in its own
snapshot. An external invocation's request keeps its type and params in the
region, so a snapshot keeps them too.

## Trace policy (ADR 0007)

A context's detail class is in its flags, so it is kept wherever the
context is: the region, hibernation, and export blobs (which carry the
flags whole). The compiled rules and their strings are in the region, so a
snapshot keeps them and a restore runs under them until the host sets
another policy. An actor's cached name matches (`trace_names`) are in its
record and are cleared whenever the policy changes; an adopted actor
computes its own.
