# ADR 0079: Core tells the host when a definition is registered, retired or freed

**Status:** Accepted; not yet implemented. Refines ADR 0043 (retirement) and ADR 0077 (holds and freeing). Serves napi ADR 0024.
**Date:** 2026-09-26

## Context

Since ADR 0077, a definition's lifetime is core's: it is freed when no
hold, no nesting parent and no resident actor is left, and that can happen
long after, and far from, the host call that registered it. The last child
of an invoke ending, a parent's cascade, or `ta_system_retire_definition`
followed by an actor finishing all free definitions the host did not name
in that call. Core also registers definitions the host never registered
itself: the nested children an import compiles (ADR 0077).

A host that keeps something per definition cannot tell when to let it go.
napi keeps each definition's `resolve(src)` function (napi ADR 0024) and
releases them with a periodic sweep that calls `ta_definition_inspect` on
every kept ID until one answers `TA_ERR_NOT_FOUND`. That is a poll for an
event core already knows about, it runs in a host call that has nothing to
do with the freed definition, and it keeps the function alive until the
next sweep. Any other per-definition host state (caches, name tables in an
exporter, metrics bookkeeping) would need a sweep of its own.

## Decision

### 1. A definition sink, in the style of the trace sink

```c
typedef enum {
    TA_DEFINITION_EVENT_REGISTERED = 0, /* a new registration, not a reuse   */
    TA_DEFINITION_EVENT_RETIRED,        /* ta_system_retire_definition       */
    TA_DEFINITION_EVENT_FREED           /* its storage and ID go after this  */
} ta_definition_event_kind;

typedef struct {
    ta_definition_event_kind kind;
    ta_definition_id definition;
    ta_definition_identity identity;    /* borrowed for the call             */
} ta_definition_event;

typedef struct ta_definition_sink_vtable {
    void (*notify)(void *self, TA_IN const ta_definition_event *event);
} ta_definition_sink_vtable TA_VTABLE;

typedef struct {
    void *self;
    const ta_definition_sink_vtable *v;  /* never null                     */
} ta_definition_sink TA_ADAPTER;

TA_API ta_definition_sink ta_null_definition_sink(void);
TA_API ta_result ta_system_set_definition_sink(ta_system *system, ta_definition_sink sink);
```

A system starts with `ta_null_definition_sink()`, whose `notify` does
nothing (ADR 0057). The setter refuses a sink whose `v` or `v->notify` is
null with `TA_ERR_INVALID`, so a call site calls the sink with no test, as
ADR 0067 section 2 does for the trace sink.

**A sink, not a trace record kind.** Trace records (ADR 0067) belong to a
session's steps: they carry a step key, a position and a trace context,
they are masked and sampled per macrostep, and a host turns them into
spans. A definition has none of these: it is not an actor, it has no step,
and its lifecycle is not caused by one request. Cleanup also cannot depend
on a mask or on sampling: a host that keeps a function per definition must
hear about every free. So the lifecycle is one always-on adapter, and a
host that wants to see definition lifecycles in its telemetry emits logs
or events from its own `notify`. Records as well would be a second way to
say the same thing.

**All three events, not only FREED.** They cost nothing extra: each fires
where core already does the work, a few times per definition, never per
event. REGISTERED is the only way a host learns about definitions core
registers on its own (an import's nested children, ADR 0077), and pairs
with FREED for any host that keeps per-definition state from the start.
RETIRED lets a host stop offering a definition (a name table, a
`findDefinition` cache) at the moment core stops.

### 2. When each event fires

- **REGISTERED:** in `ta_system_register_definition`, after a new
  definition takes its slot and its first hold. A registration answered
  by an existing definition (ADR 0077's reuse by identity) adds a hold and
  fires nothing: no definition came into being.
- **RETIRED:** in `ta_system_retire_definition`, the first time a
  definition is retired. Retiring again, or retiring a freed ID, fires
  nothing.
- **FREED:** when core frees a registered definition, before its storage
  goes and before its ID goes stale:
  - **Order in a cascade:** freeing a definition can free definitions it
    nests, and those can free theirs (ADR 0077). Core first collects the
    whole cascade with its worklist, then fires FREED and frees in reverse
    order of discovery. A nested definition is discovered only after the
    definition nesting it, so **leaves are freed first**, and every
    definition is still intact when its FREED fires. Siblings come in no
    promised order. No recursion (ADR 0060).
  - **At `ta_system_destroy`:** FREED fires for every registered definition
    still alive, after every actor is gone, in slot order. A host can
    therefore release per-definition state in `notify` alone, with nothing
    left over at close.
  - A definition that was compiled but never registered fires nothing:
    the host holds it, and destroying it is the host's call.

### 3. What the host may do in `notify`

- **Read:** during every event the definition is intact and its ID
  resolves: `ta_definition_inspect`, `ta_definition_state`, the ADR 0071
  lookups and `ta_definition_export` work, and `identity` is the
  definition's own storage, borrowed for the call. A host finds whatever it
  keyed by the ID or the identity.
- **Not change definitions or actors:** no registering, retiring or
  releasing a definition, no spawning, destroying or resuming an actor,
  no delivering. The event may fire inside any of those calls (a cascade
  inside `ta_actor_destroy`, the last child ending inside a resume, the
  whole teardown inside `ta_system_destroy`), where core is between states.
  This is the trace sink's rule (ADR 0067 section 1), stated for
  definitions.
- **Cannot fail:** `notify` returns nothing, as `write` does.

### 4. Cost

Nothing on the hot path: no event, delivery or microstep reaches the sink.
Each registration, retirement and free makes one indirect call. The freeing
cascade reuses the definition's existing `free_next` link for its second
pass. `ta_system` grows by one sink (two pointers, 16 bytes); nothing grows
per definition or per actor.

## Consequences

- napi installs a sink when the system is created and releases a kept
  `resolve` on FREED. The periodic sweep, its mark and its comment go.
  napi's close no longer walks the resolvers: teardown's FREED events have
  released them.
- Any future per-definition host state (an exporter's name table, a
  metrics series that should end with its definition) uses the same sink,
  not a sweep.
- Hosts learn about core-registered nested children the moment they exist.

## Testing

- **Core:**
  - the null sink is installed at creation, and the setter refuses a null
    `v` or `notify`;
  - REGISTERED once for a new registration, none for a reuse by identity;
  - RETIRED once, not for a second retire;
  - FREED once for a release that frees; not while an actor or a nesting
    parent still uses the definition; then when the last actor goes;
  - a nested cascade fires FREED leaves first, and the definition is
    inspectable inside `notify`;
  - `ta_system_destroy` fires FREED for every definition still alive.
- **napi, on Node and Bun:** a definition's `resolve` is released exactly
  when the definition is freed: not while a child it invoked still runs,
  and at once when the last one ends.
