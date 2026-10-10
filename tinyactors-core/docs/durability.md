# Durable state of a session

One definition of what a session *is* when it is not running, and of what
every persistence path keeps of it. A path that drops part of it changes what
the session does next; `tests/unit/test_durable.c` checks that none does.

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

| # | hibernate → wake | export → adopt | + `ta_blob_migrate` | system / host snapshot |
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
| 17 | ✗ | ✗ (external invocations cannot move) | ✗ | ✓ |
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
  (the next `ta_system_run` drains them) and exports then.
- **Snapshots** are the region bytes plus the datamodel's own snapshot; they
  need a drained outbox (`ta_system_snapshot`) or a quiescent host (effects
  acknowledged, no migration job: `ta_host_snapshot`). Hibernation blobs stay
  with the host and are not part of a snapshot.
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
| JS bindings (node, wasm) | tagged JSON codec of: the xstate datamodel's context, event and input; the ECMAScript datamodel's variables + `_event` + invoke params (`#init`) and, for charts whose scripts declare globals, `let`/`const`/`class` bindings (`#lex`: value, const-ness or uninitialized) and top-level function declarations (`#fns`, re-created on restore). Functions in data, classes, Errors, Promises ✗ — the error names the variable, the path inside it and a function's document line | values (`payload_serialize`, the same codec): `system.export(actor)` → `system.adopt(bytes)` in another process | the bindings' value tables |

A blob of the C ECMAScript datamodel is a value: another process, or the same
one after a restart, can adopt it (the PostgreSQL binding stores one per actor
and loads it in whichever backend processes the actor next).

So is an export blob of the JS bindings: `system.export(actor)` wraps it with
what the actor runs (machine SCXML, documents or image bytes) and the
exporter's clock; `system.adopt(bytes)` in any system (another process, Node or
web) adds that segment if it has no identical one, rebases the delayed sends
to its own clock (`ta_blob_rebase`: each keeps the time it had left) and adopts
it through `ta_host_adopt`, which routes to the session in whatever segment
took it and keeps the segments of its home id range from handing its id out
again.

## Blob formats

Unreleased (v0.1.0): blob formats change freely, and are for one build of the
library (instance words are copied verbatim).

- **hibernation** (`HIBERNATION_VERSION 2`): header | instance words | invoke ids as
  text | datamodel bytes.
- **export** (`EXPORT_VERSION 2`): header | per session, parents first: header |
  instance words | invoke ids | datamodel bytes | delayed sends (absolute due,
  rank in the tree's firing order, route, slot + 1 for `#_<invokeid>`,
  target/name/sendid text, payload bytes or handle) | `<data>` to initialize
  (migrated) | name.

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
a hibernation or an export does not move them. The snapshot format (`ta_system`
`version`) and the hibernation and export blobs (`HIBERNATION_VERSION`,
`EXPORT_VERSION`) changed with the 64-byte instance header and the contexts of
delayed sends: older ones are refused.

## Per-chart counts (ADR 0007)

The counts of each chart (`ta_chart_stats`) are system state, not session
state: they live in the region, so relocation and a system snapshot keep
them, and a hibernation or an export leaves them where they were made. A host
keeps the totals of the segments it retired, by chart name, in its own
snapshot. The snapshot formats (`ta_system` `version` 3, the host's
`SNAPSHOT_VERSION` 2) changed with them: older ones are refused.

## Trace policy (ADR 0007)

A context's detail class is in its flags, so it is kept wherever the
context is: the region, hibernation, and export blobs (which carry the
flags whole). The compiled rules and their strings are in the region, so a
snapshot keeps them and a restore runs under them until the host sets
another policy. An actor's cached name matches (`trace_names`) are in its
record and are cleared whenever the policy changes; an adopted actor
computes its own.
