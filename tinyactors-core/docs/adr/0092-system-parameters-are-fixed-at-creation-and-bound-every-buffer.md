# ADR 0092: System parameters are fixed at creation and bound every buffer

**Status:** Proposed (2026-09-29). Would amend:
- ADR 0060: interpreter frames and scratch are sized by a parameter, not per definition, and a chart that needs more is refused when it is defined;
- ADR 0075: the microstep limit is a parameter, and each definition's limit defaults to it;
- ADR 0023: every buffer comes from the provided allocator at creation.

napi ADR 0035 applies it to the addon.
**Date:** 2026-09-29

## Context

Applications differ in what they need from a system, but the knobs that
express this are scattered and inconsistent:
- **Some are runtime setters with no bounds:** `ta_system_set_microstep_limit()`,
  `ta_system_set_event_block_cache()`.
- **Some are `#define`s that also size arrays on the stack:**
  - `TA_EVENT_NAME_MAX_DEPTH 64` (`ta_atom out[TA_EVENT_NAME_MAX_DEPTH]` in
    `src/event_name.c`), which napi mirrors as its own `TNA_EVENT_NAME_MAX_DEPTH`;
  - `TA_COMPILE_ERROR_MESSAGE_MAX`, `TA_COMPILE_ERROR_TEXT_MAX` and
    `TA_COMPILER_NEAREST_MAX`.
- **Some are hidden inside hosts.** napi cuts trace values at 1023
  characters because of a fixed `char trace_value[1024]`. granary needs 16
  KiB.

Memory is also spread across the run:
- `system->scratch` is an arena that grows in 4 KiB blocks
  (`src/system.c`);
- interpreter frames and sets are sized per definition, from its state
  count, transition count, history slots and depths (`ta_interpreter_init`,
  ADR 0060);
- each growth is an allocation that can fail mid-run, with the out-of-memory
  path that brings (ADR 0070).

`system->scratch` exists for four reasons, and they constrain this ADR:
1. **Actors carry no interpreter memory.** A macrostep's bitsets,
   transition lists, entry-set frames, execution frames and trace buffers
   live in one arena the system shares, so an idle actor holds none of them.
   That is how an actor stays at 332 bytes up to 4 million actors. A paused
   actor moves its frames out and back (ADR 0061).
2. **Nothing is allocated per run.** Each run resets the arena with a
   pointer move. It grows only to the high-water mark of the largest chart
   it has run, and stays there.
3. **Memory follows the chart.** A run takes what its definition needs
   (ADR 0060), so a small chart pays little.
4. **Nested interpreters get arenas on demand.** Cancelling an invocation
   exits the child's session inside the parent's interpreter, with an
   arena of its own allocated only then (`src/interpreter.c`).

ADR 0068 also made it the home of **temporaries whose size depends on
content**: evaluated `<param>` arrays, and text rendered for the data
model (origins, `_ioprocessors` locations, event names, send IDs). A
compile-time check cannot bound those, because it cannot see how long a
name will be.

After review, four rules were accepted.
1. **Parameters are set once, at creation, and never change.** Sink
   configuration (trace ratio, detail, kinds) and per-definition overrides
   are not parameters.
2. **Bounds on input and output are parameters like any other.** Internal
   layout constants are not.
3. **Every buffer is bounded by a parameter,** and comes from the provided
   allocator. Buffers that every run needs are allocated at creation.
   Buffers that only nested interpreters need are allocated on first use,
   up to their bound, and kept. Storage that grows with population stays
   dynamic, and is bounded by quota parameters. (The first form of this
   rule reserved every buffer at creation. Review showed that this gives up
   reasons 3 and 4 above for the worst case, and makes content-sized
   temporaries fail by content in a fixed region, so it was amended.)
4. **Defaults are enumerators** (`TA_CONSTANTS_OF`) that fit in 31 bits. A
   runtime info table is what hosts read.

## Decision

### 1. The parameters

```c
typedef enum {
    TA_PARAM_MICROSTEP_LIMIT,          /* microsteps per macrostep before a runaway (ADR 0075); 0: none */
    TA_PARAM_EVENT_BLOCK_CACHE,        /* event blocks kept for reuse (ADR 0069), allocated at creation */
    TA_PARAM_EVENT_NAME_DEPTH,         /* segments in an event name */
    TA_PARAM_COMPILE_MESSAGE_BYTES,    /* bytes kept of a data model's rejection message */
    TA_PARAM_COMPILE_TEXT_BYTES,       /* bytes of a formatted compile error */
    TA_PARAM_COMPILE_NEAREST_BYTES,    /* longest unknown target that gets a suggestion */
    TA_PARAM_INTERPRETER_BYTES,        /* one interpreter's frames and sets: what the structure determines */
    TA_PARAM_RENDER_BYTES,             /* one run's temporaries whose size depends on content */
    TA_PARAM_INTERPRETER_NESTING,      /* interpreters running inside one another (cancelling an invocation) */
    TA_PARAM_MAX_ACTORS,               /* quota: resident actors */
    TA_PARAM_MAX_PENDING_SENDS,        /* quota: pending delayed sends of one actor */
    TA_PARAM_MAX_DEFINITIONS,          /* quota: live definitions */
    TA_PARAM_COUNT
} ta_param TA_SYMBOLS("TA_PARAM_");

enum TA_CONSTANTS_OF(uint32_t, "TA_PARAM_DEFAULT_") {
    TA_PARAM_DEFAULT_MICROSTEP_LIMIT = 100000,
    TA_PARAM_DEFAULT_EVENT_BLOCK_CACHE = 4,
    TA_PARAM_DEFAULT_EVENT_NAME_DEPTH = 64,
    TA_PARAM_DEFAULT_COMPILE_MESSAGE_BYTES = 256,
    TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES = 1024,
    TA_PARAM_DEFAULT_COMPILE_NEAREST_BYTES = 128,
    TA_PARAM_DEFAULT_INTERPRETER_BYTES = 262144,
    TA_PARAM_DEFAULT_RENDER_BYTES = 16384,
    TA_PARAM_DEFAULT_INTERPRETER_NESTING = 4,
    TA_PARAM_DEFAULT_MAX_ACTORS = 2147483647,
    TA_PARAM_DEFAULT_MAX_PENDING_SENDS = 2147483647,
    TA_PARAM_DEFAULT_MAX_DEFINITIONS = 2147483647
};
```

- **The defaults are today's values.** The microstep limit takes napi's
  100,000, which the test host adopts too. The quotas default to the
  largest value, which is no limit.
- **`TA_PARAM_DEFAULT_INTERPRETER_BYTES` and `TA_PARAM_DEFAULT_RENDER_BYTES`
  are provisional.** The
  implementation sets it from the largest chart in the repository's suites
  (the IRP suite, and napi's and the test host's charts), with headroom, and
  records the figure.

### 2. Setting them: at creation, once

```c
typedef struct { ta_param param; uint64_t value; } ta_param_value;

typedef struct {
    ta_allocator allocator;
    ta_time initial_time;
    uint64_t incarnation;
    TA_COUNTED_BY(param_count) TA_OPTIONAL const ta_param_value *params;
    uint32_t param_count;
    TA_OUT TA_OPTIONAL ta_param *rejected;   /* the parameter a TA_ERR_INVALID refers to */
} ta_system_config;

TA_API ta_result ta_system_param(const ta_system *system, ta_param param, TA_OUT uint64_t *out);

typedef struct { ta_str name; uint64_t default_value; uint64_t min; uint64_t max; ta_str unit; } ta_param_info;
TA_API ta_result ta_param_info_of(ta_param param, TA_OUT ta_param_info *out);
```

- **Creation validates every parameter.** A value outside `[min, max]`, or
  a parameter listed twice, answers `TA_ERR_INVALID` with `rejected` set,
  and creates nothing.
- **Parameters cannot change after creation.** `ta_system_param()` reads
  one back.
- **`ta_param_info_of()` is how hosts learn a parameter's name, bounds and
  unit.** Names are lowerCamelCase (`microstepLimit`), which hosts use as
  they are.
- **Removed:** `ta_system_set_microstep_limit()` and
  `ta_system_set_event_block_cache()`. Their callers migrate to parameters.
  `ta_definition_set_microstep_limit()` stays, as a property of a
  definition. A definition's limit of 0 means the system's parameter.
  `ta_system_set_trace_ratio()` stays, as configuration of the trace sink.

### 3. Every buffer is bounded; the ones every run needs exist from creation

`ta_system_create()` computes the size of the system's arena from its
parameters, and allocates it from the provided allocator in one call. It
holds:
- **Interpreter region 0:** `INTERPRETER_BYTES`, for the frames and sets
  whose size the chart's structure determines (ADR 0060). It replaces the
  growing `system->scratch` for every ordinary run. Reset is a pointer
  move.
- **The render area:** `RENDER_BYTES`, for one run's temporaries whose size
  depends on content (ADR 0068). It is reset with region 0.
- **Event-name segment buffers:** `EVENT_NAME_DEPTH` atoms, replacing the
  arrays on the stack.
- **Compile-error buffers:** message, text and suggestion.
- **The event-block cache:** `EVENT_BLOCK_CACHE` blocks, created up front.
- **The input-log record scratch** of ADR 0090: a fixed size, since its
  largest fixed part is known.

If the allocation fails, creation answers `TA_ERR_NOMEM` and creates
nothing. None of these buffers can run out of memory during a run.

**Nested interpreters' regions are allocated on first use.** Regions 1 to
`INTERPRETER_NESTING − 1`, each of `INTERPRETER_BYTES` with a render area
of its own, are allocated when an invocation is first cancelled that deep,
and are then kept and reused, not freed. So:
- memory stays bounded by `INTERPRETER_NESTING × (INTERPRETER_BYTES +
  RENDER_BYTES)`;
- a system that never cancels an invocation inside another pays for one
  region;
- failing to allocate a nested region keeps today's behaviour for that
  path. The child is exited later, as `ta_interpreter_exit_invoked` handles
  a failed arena now;
- nesting deeper than `INTERPRETER_NESTING` is refused at `define()` when
  the invocation's chart is known. Otherwise it is an execution error when
  the invocation starts.

**The structural region is checked when a chart is defined.** The compiler
already computes what a definition's interpreter needs: sets sized by
state and transition counts, frames sized by the recorded maximum action
nesting and state depth. It compares that with `INTERPRETER_BYTES`. If the
chart needs more, it is a compile error (ADR 0058) naming both figures:

> `the chart needs 301,120 bytes of interpreter scratch; this system has 262,144 (interpreterBytes)`

So a chart that defines never runs out of structural scratch.

**The render area is checked as a run uses it.** A temporary that does not
fit is `error.execution` in the running action, with data naming
`renderBytes`, as a failed scratch allocation for `<param>` values is
today. It is deterministic, since the same content fails the same way, and
it is documented as the one limit that depends on content.

An event name deeper than `EVENT_NAME_DEPTH` is `TA_ERR_INVALID` where it
is interned, as with 64 today. This replaces ADR 0060's "depth costs heap
memory, not a limit": depth is now bounded by a parameter the application
chooses.

### 4. What grows is bounded by quotas

Actors, definitions, pending sends, event data, continuations and values
grow with population and content, so they keep the allocator. The quota
parameters bound them:
- **a spawn beyond `MAX_ACTORS`** answers `TA_ERR_NOMEM`, as an allocator
  that ran out does. A host treats both the same, and an operator sees which
  one it was in the counters;
- **a delayed send beyond `MAX_PENDING_SENDS`** for its actor raises
  `error.execution` in the sender;
- **a definition beyond `MAX_DEFINITIONS`** is refused at registration.

### 5. Not parameters

These stay compile-time constants (rule 2):
- page and bucket sizes (`TA_ACTOR_PAGE_SLOTS`, `TA_TIMER_PAGE_OWNERS`, the
  address and session tables, `TA_TREE_BLOCK_SIZE`);
- `TA_IO_ORIGIN_INLINE`, `TA_EVENT_BLOCK_QUEUE_INITIAL` and
  `TA_COMPILER_LINEAR_NAMES`.

They feed layout arithmetic, and no application requirement asks to
change them.

## Consequences

- **Every buffer is bounded by a parameter.** Those every run needs are
  allocated at creation, and never grow. Nested regions appear only on first
  use, up to their bound.
- **The four reasons for `system->scratch` still hold:** actors carry
  nothing, a run allocates nothing, a small chart's system can choose a small
  `interpreterBytes`, and nesting costs memory only when it happens.
- **A system's parameters are one fact.** A host can put them in the
  telemetry scope, and store them beside an input log. That record matters
  most for `microstepLimit`, the one parameter that changes behaviour.
- **Behaviour changes:**
  - a chart needing more interpreter scratch than the parameter allows is
    refused at `define()`;
  - a run whose content-sized temporaries exceed `renderBytes` raises
    `error.execution`, where the arena used to grow;
  - the microstep limit and the event-block cache can no longer change on a
    running system;
  - spawns, sends and definitions can hit quotas.
- **Callers change:**
  - napi's `scheduling.microstepLimit`, and its calls to the removed
    setters;
  - the test host's settings;
  - `ta_system_config`'s initialisers everywhere.
- **Costs:**
  - per system with the defaults: 272 KiB reserved at creation (region 0
    and its render area). Each nesting level used adds the same, up to
    about 1.1 MiB at `interpreterNesting` 4;
  - a host running many small systems, such as napi's test suites, lowers
    `interpreterBytes` to what its charts need;
  - the rest is a few KiB;
  - per actor, per event and per send: nothing;
  - the implementation measures and records the arena's size with the
    defaults, before and after.

## Considered and rejected

- **Reserving every nesting region at creation** (this ADR's first form).
  With 16 regions of 256 KiB it cost 4 MiB per system, for a case most
  systems never meet. That worked against the memory bounds it was meant to
  serve.
- **One region for structural and content temporaries.** A check at
  `define()` could then never be complete, because a long name could
  exhaust the region for a chart the compiler had accepted. Two areas make
  the structural guarantee exact, and the content limit explicit.
- **Parameters that can change at run time.** Buffers sized by a parameter
  would have to grow or be revalidated, and a behaviour-changing limit could
  differ between a live run and its replay.
- **Making layout constants parameters.** Division instead of shifts, for a
  knob no requirement asks for.
- **`#define` defaults.** The compiler and the API generator do not see
  them, which is how napi came to keep a second copy of the event-name
  depth.
- **A separate knob per interpreter table.** One byte budget, checked
  against the need the compiler already computes, is one number to reason
  about, and the error says exactly how far a chart is over.

## Implementation notes

None yet.
