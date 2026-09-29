# ADR 0092: System parameters are fixed at creation and bound every buffer

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Nesting past `interpreterNesting` is refused when the child is spawned (`TA_ERR_UNSUPPORTED`), not at `define()`, and `eventNameDepth` can only be lowered (8 to 64); see the notes. Refined by ADR 0093: the event-block cache fills on demand up to `eventBlockCache` (default 1024) instead of being created at creation. Amends:
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

- **Where:** `include/tinyactors.h` (`ta_param`, the `TA_PARAM_DEFAULT_*`
  enumerators through `typedef uint32_t ta_param_default`, `ta_param_value`,
  `ta_param_info`, `ta_system_config.params/param_count/rejected`,
  `ta_system_param()`, `ta_param_info_of()`, `ta_counters.quota_refusals`);
  `src/system.{c,h}` (the parameter table, validation, the creation arena,
  nested regions, quotas for actors and definitions); `src/arena.{c,h}`
  (`ta_arena_init_fixed`: an arena over caller memory whose backing refuses,
  so no use site changed); `src/interpreter.{c,h}` (the render area reset at
  interpreter start and at every unit, `ta_interpreter_scratch_need`, the
  cache filled at creation, nested exit frames, the pending-send quota);
  `src/builder.c` (the check at compile time, and the message and nearest
  limits from parameters); `src/invoke.c`, `src/datamodel.c`,
  `src/input_log.c` (content-sized temporaries in the render area);
  `src/actor.c` (the actor quota, the nesting check at spawn);
  `src/event_name.{c,h}` (`max_depth` on the table). Tests:
  `tests/params_test.c` (new), `tests/deep_test.c`, and the tests that used
  the removed setters.
- **Parameters that are not in the list:** `compileTextBytes` is not a
  parameter. `ta_compile_error_format()` takes no system, writes into the
  caller's arena, and its per-field limits are what bound its line, so
  `TA_COMPILE_ERROR_TEXT_MAX` stays a constant. `TA_COMPILE_ERROR_MESSAGE_MAX`
  and `TA_COMPILER_NEAREST_MAX` are removed in favour of
  `compileMessageBytes` and `compileNearestBytes`.
- **`eventNameDepth` can only be lowered** (bounds 8 to 64). Segment arrays
  stay on the stack, sized by the compile-time ceiling
  `TA_EVENT_NAME_MAX_DEPTH`; moving them into shared buffers would have made
  helpers that call one another share one array. The table checks the
  parameter where names are interned.
- **Bounds:** microstepLimit 0 to 2^31−1 (0: none); eventBlockCache 0 to
  65,536; compileMessageBytes 16 to 65,536; compileNearestBytes 0 to 65,536;
  interpreterBytes 1,024 to 1 GiB; renderBytes 256 to 1 GiB;
  interpreterNesting 1 to 64; the quotas 1 to 2^31−1.
- **The render area is reset at every unit of executable content**, not only
  when an interpreter starts: a `<foreach>` of many sends with `<param>`s
  would otherwise fill it with arrays nothing reads any more. Everything it
  holds (param arrays, rendered text, an invoke ID's text, an I/O origin's
  URI for the input log) is used within the unit that made it, or within
  the step outside units that made it.
- **Nested regions:** an exit frame at depth d (from 1) takes region d, a
  structural region and its own render area in one allocation made the
  first time and kept until the system goes. An invoked child whose
  invoking chain would need a region past `interpreterNesting` is refused by
  `ta_actor_spawn()` with `TA_ERR_UNSUPPORTED`, which the host's invoker
  reports; the ADR's "refused at define() when the invocation's chart is
  known" is not done, since core does not know which chart an invoker will
  spawn.
- **Quotas** answer `TA_ERR_NOMEM` for an actor or a definition past its
  quota, and raise `error.execution` (code `TA_ERR_NOMEM`) in the sender for
  a delayed send past `maxPendingSends`; each counts one `quota_refusals`.
- **The structural need is exact.** `ta_interpreter_scratch_need()` repeats
  `ta_interpreter_init`'s allocations in order and alignment, both trace
  buffers included. `params_test` defines a 63-state parallel chart with
  history and guards, reads the need from the compile error, creates a
  system with exactly that many `interpreterBytes` and runs the chart with
  every trace kind; `deep_test` does the same for 10,000 levels of actions.
- **Measured need:** across every chart napi's suites and the W3C IRP suite
  define (4,097 definitions), the largest structural need is 3,216 bytes, for
  101 states (about 32 bytes per state; the mean is 644). The default
  interpreterBytes of 256 KiB fits about 8,000 states, 80 times the largest
  chart in the repository. renderBytes keeps 16 KiB: IRP runs 176/176 with
  it. napi's one test with a 200,000-character event name
  (`test/datamodel.js`) now expects the deterministic render error.
- **Costs**, measured with a counting allocator:
  - a default system allocates 297,048 bytes at creation, against 15,104
    before: the creation arena is 278,656 bytes (region 0 of 262,144 plus its
    header, the render area of 16,384 plus its header, the table of 3 nested
    regions), the 4 cached event blocks are 792 bytes each, and the system record grew by the parameters. Before, the
    scratch arena grew in 4 KiB blocks on first use and the cache filled as
    blocks came back;
  - per actor: 0 bytes (`scale_test` 331 bytes per actor, flat to 200,000);
  - each nested level used: one region of 278,608 bytes, allocated once;
  - napi passes `scheduling.messageBudget` (1,024 by default) as
    eventBlockCache, so a napi system creates 1,024 blocks, about 811 KB, at
    creation, where it used to fill them under load.
- **Callers:** napi's `tna_system_create()` takes a params struct; the
  microstep limit and the block cache are read from `createSystem` options
  before the core system is created (`tna_pump_core_params`); the native
  tests pass `TA_PARAM_MICROSTEP_LIMIT` where they set it after creation.
  The test host's `scale_test` measures the block cache by creating systems
  with the parameter.
- **Addendum (2026-09-29): the default interpreterBytes is 64 KiB.** The
  implementation measured the largest structural need at 3,216 bytes (mean
  644) and still kept the provisional 256 KiB, which made every system
  reserve almost 300 KB at creation. The Decision's own rule is "the largest
  chart, with headroom": 65,536 bytes is about 20 times the largest chart,
  room for about 2,000 states. A default system now allocates 100,632 bytes
  at creation (measured with a counting allocator), against 297,048 with 256
  KiB and 15,104 before this ADR. A chart that needs more is refused at
  `define()` with the figure to set.
- **Addendum (2026-09-29): `compileTextBytes` is a parameter.** This
  reverses the deviation above. `ta_compile_error_format_params` gains an
  optional `system`: with one, the line is at most that system's
  `compileTextBytes` (default 1024, 256 to 65,536); without one, the
  default. The field limits derive from the size: IDs, subject and related
  text a sixteenth, the message three thirty-seconds, and the path the rest
  after their worst case, a quarter of its steps at the head. At 1024 bytes
  these are the formatter's old limits (64, 96, and 2 and 6 steps); at 256
  it shows 1 and 2 steps and 16-byte IDs; at 8192, 2 and 9 steps and 512-byte
  IDs. `TA_COMPILE_ERROR_TEXT_MAX` is removed, and its callers pass their
  system: napi `src/import.c`, `tinyactors-scxml/tests/ta_scxml_test.c`, and
  core's `span_test.c` (`compile_error_test.c` checks against the default).
  `compile_error_test` formats a 60-step path with 20,000-byte texts at 256,
  1024 and 8192 bytes, each within its size and showing more at each size.
  napi still keeps at most 1,023 bytes of the line in its refusal
  (`tna_import_failure.message`).
