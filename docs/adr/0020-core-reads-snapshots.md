# ADR 0020: Core reads snapshots, without a host

**Status:** Accepted in outline (2026-11-26); refined the same day after a
refutation (a wrapped journal, the tick rate, what marks a system to
read, the lifetimes and the trust a reading system needs).
Decided by the maintainer: readers
over a snapshot's bytes, which need no host ("Why must the host be frozen
for reading a bunch of bytes?"), made public in C and mirrored by the
bindings. It replaces the first draft of the same day, which restored a
host that never ran (a "frozen" host), added methods, and added keys to
`actor.get`. All of that is withdrawn.
**Date:** 2026-11-26

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the operator, through a tool that reads a snapshot.** The
  tools are the MCP server (`@tinyactors/mcp`), `tinyactors inspect` and
  `tinyactors actors`. The operator asks what every actor was doing at one
  moment:
  - its state, its mailbox and its delayed sends;
  - the versions and their charts;
  - the spans before that moment.

  Today the CLI answers with code of its own
  (`tools/tinyactors/snapshot.c`). The MCP server answers with a copy of
  the website Inspector's JavaScript decoder.
- **Then: the binding author,** who mirrors C readers instead of writing a
  decoder.
- **Out of scope:**
  - **A snapshot of another build.** The website Inspector reads one in
    JavaScript, from the layout description the pack carries, and keeps
    its decoder. Core's region is raw memory of one build, and core reads
    its own layout only.
  - **The variables.** They are the datamodel's state: a binding's own
    encoding, or quickjs's. A tool reads them from a host restored from the
    snapshot, as now.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event, per actor | nothing |
  | memory | a reader holds a copy of one segment's region, in memory its caller gives (in a binding: a `Buffer` or a `bytearray`) |
  | API | `ta_host_snapshot_region`; `ta_system_read`; three readers made public (`ta_system_actor_name`, `_mailbox`, `_timers`); `ta_system_stats.ticks_per_second`; a chart's transitions (`ta_image_transition_*`); the journal's kept records (`ta_system_trace_kept`) and their spans (`ta_otlp_history`); a row for each in `docs/boundary.md`, and embedding.md section 12 |
  | memory per read | a copy of each segment's region read, beside the pack the tool keeps |
  | bindings | each mirrors those calls, plus the existing readers (`ta_system_actors`, `ta_actor_info_get`, `ta_actor_config`, the image's) |

## Context

1. **The CLI** (`tools/tinyactors/snapshot.c`, `restore`):
   - unpacks a pack;
   - binds each datamodel its images name to a vtable that does nothing;
   - restores a whole host and never runs it;
   - reads each segment's system with the public readers
     (`ta_system_actors`, `ta_actor_info_get`, `ta_actor_config`) and the
     internal ones (`ta_system_actor_mailbox`, `_timers`, `_name`).

   It needs a host only to get from the pack to each segment's system.
2. **A segment's system can be read without a host.** `ta_system_restore`
   copies a region into memory its caller gives, checks the layout id and
   the image, and binds the datamodels (`src/system/system.c`). Its
   readers take a `const ta_system *`.
3. **The pack's other parts already have host-free calls**
   (`docs/requests.md`, "Not methods": calls that need no host):
   - `ta_host_snapshot_unpack` gives the header;
   - `ta_host_snapshot_image` gives segment i's image.

   (`ta_host_snapshot_segments` reads a host snapshot, not a pack.) Only
   the regions' bytes have no call.
4. **The Inspector turns the journal's kept records into spans** in
   JavaScript, named as core's OTLP encoder names them. That includes the
   acknowledged records the snapshot keeps for that purpose (`4255811c`).

## Decisions

1. **`ta_bytes ta_host_snapshot_region(ta_bytes pack, uint32_t segment)`:**
   segment i's region in the pack, a view into the pack. It is empty for a
   retired segment or a pack that does not unpack.
2. **`ta_status ta_system_read(const ta_system_setup *setup, ta_bytes
   region, ta_system **out)`** makes a system to read.
   - **What it does.** It copies the region into `setup->memory`, at least
     the region's length and 8-aligned. It refuses another layout or
     another image, as `ta_system_restore` does.
   - **Datamodels.** It binds no datamodel: `setup->datamodels` must be
     empty. Every chart's binding is the null datamodel's, no chart is
     attached, and the system's datamodel count is 0. That count marks a
     system to read:
     - `ta_system_run` refuses it (`TA_RUN_FAULT`, `TA_ERROR_STATE`);
     - `ta_system_deinit` leaves it;
     - every payload's datamodel is past the count, so nothing calls a
       datamodel.
   - **What reads it.** The system's readers only. Nothing is sent to it.
   - **Lifetimes.** The system holds pointers into itself and to the
     `ta_image`, so its memory, the `ta_image` and the image's bytes must
     outlive every view and must not move.
   - **Trust.** The region must be one this build wrote. Its records are
     trusted, as a restore trusts them, so a binding keeps the copy
     private.
   - **Its end.** It needs no `ta_system_deinit`: its caller frees the
     memory.
   - **How it differs from `ta_system_restore`.** It takes a datamodel for
     no chart.
3. **Three readers become public** (`include/tinyactors/system.h`), as
   they are:
   - `ta_system_actor_name`;
   - `ta_system_actor_mailbox` (`ta_actor_queued`);
   - `ta_system_actor_timers` (`ta_actor_timer`).

   Each is a view of region strings, valid while the system's memory is,
   and none runs anything. `ta_system_stats` gains `ticks_per_second`, the
   rate of its `now` and of a delayed send's due, which no public call gave
   before.
4. **A chart's transitions** (`include/tinyactors/image.h`), as its
   states (`ta_image_state_info`) and its data (`ta_image_data`) already
   are:
   - `ta_image_transition_count(image, chart)`;
   - `ta_image_transition(image, item, ta_transition_info *)`: its source,
     its type (external, internal, initial), its condition (an expression,
     `TA_NIL` for none), how many events and targets it has;
   - `ta_image_transition_event(image, item, i)`: the name or prefix an
     event descriptor matches, `*` for any. The compiler keeps only the
     matched node of the event trie, so `order`, `order.` and `order.*` all
     read `order`;
   - `ta_image_transition_targets(image, item, ta_uint32_array)`.
5. **The journal's kept records,** `ta_status ta_system_trace_kept(const
   ta_system *system, ta_trace_kept *out)`: the records the journal keeps,
   oldest first. Those are the acknowledged ones it retains, then the
   pending ones.
   - **Pieces.** A wrapped journal is not contiguous, so the records come
     in two views, `first` and `second`.
   - **The acknowledged.** `acknowledged` is the bytes of the acknowledged
     records, counted from the first record through both views.
   - **Counts.** The records, how many of them are acknowledged, and the
     records lost ever.
   - **Rate.** The system's `ticks_per_second`.

   It refuses `TA_ERROR_STATE` when tracing is off. The view lives as the
   system's memory does.
6. **Their spans, `ta_status ta_otlp_history(const ta_otlp_history_input
   *input, ta_buffer *out)`,** in the OTLP layer. It writes an
   ExportTraceServiceRequest of the kept records into `out`.
   - **Memory.** The encoder is allocated through the input's allocator for
     the call and freed before it returns. It grows as it needs to (open
     spans, chunks, scopes), and puts the staging's spans as it fills.
   - **Probing.** `TA_ERROR_NO_MEMORY` with `out->length` the exact bytes of
     the request, so a probe with capacity 0 sizes it, and the encoding
     runs again with the room. The puts it writes one after another make
     one request: protobuf concatenates repeated fields.
   - **Names.** Spans are named and attributed as `ta_host_traces` names
     them.
   - **Times.** A kept record has no wall time. A record's time is its
     system clock, converted with the system's `ticksPerSecond`, plus its
     fuel stamp's distance from the lowest of that tick. That is the host's
     clock, not wall time.
   - **Acknowledged records.** Their dynamic strings may have been freed,
     and their payloads released, so both are left out. Image strings
     stay, and no value is written.
   - **Spans at the edges.** A span whose beginning the ring dropped is
     left out. One still open at the end ends there, marked
     `scxml.incomplete`.
   - **Input.** `{size, source, kept, image, allocator}`. The function
     takes no system, since the OTLP layer depends on trace and image
     only. `source` is the segment's index, which names its scopes.
7. **The CLI reads through them.** `tinyactors inspect` of a snapshot and
   `tinyactors actors` go through `ta_host_snapshot_region` and
   `ta_system_read`. They no longer restore a host and drop their own
   datamodel binding. Their output stays as it is.
8. **The bindings mirror them.** Node's direct layer and Python's
   extension each get `readSnapshot(bytes)` (`read_snapshot`).
   - **What it gives.** A `Snapshot` (the header) whose `segments` are each
     a `SnapshotSegment`. A segment holds:
     - its image;
     - its system, read into a `Buffer` (`bytearray`) of the region's size;
     - its readers: `actors()`, `actor(handle)`, `configuration(handle)`,
       `mailbox(handle)`, `timers(handle)`, `name(handle)`;
     - `traces()` (the OTLP bytes) and `traceCounts`;
     - `charts`, each with its `states`, `transitions` and `data`, read
       from the image.
   - **What it holds.** Nothing of a host. The region's copy is the
     language's memory: a byte buffer the object keeps private, 8-aligned
     and never resized, gone with the object. The encoder of `traces()`
     uses the binding's allocator for the call.
9. **The MCP server reads through them,** and its copy of the Inspector's
   decoder goes. `snapshot_variables` and `rehearse` keep a host restored
   from the snapshot.

## Consequences

- A tool reads a snapshot of its own build with core's own readers, and
  no host is made to read one.
- The readers of decision 3 and the image's transitions are public ABI.
  `tests/unit/test_headers.c` checks their structs.
- A reading system is never run. Its charts have no datamodel, and it
  holds the moment of the snapshot as long as its memory lives.
