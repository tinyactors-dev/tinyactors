# ADR 0071: Transitions, actions and expressions are inspectable by ID

**Status:** Accepted and implemented; refines ADR 0042 and ADR 0049; serves ADR 0067 (closes its open point 4). See the implementation notes for where the code differs from the text.
**Date:** 2026-09-24

## Context

Trace records (ADR 0067) name what happened by compiled ID: a state, a
transition, an expression, an action. A sink turns those IDs into
something a person can read, such as an OTEL span called
`idle → busy on order.placed` instead of `transition 7`.

Core answers only one of these questions today. `ta_definition_state()`
names a state by ID (ADR 0042), and `ta_definition_find_state()` does the
reverse (ADR 0049). For a transition, an action or an expression, the
only way in is `ta_definition_export()`, which rebuilds the whole spec.
A host that wants one name would have to export the definition and
repeat the compiler's numbering to find the element, which is exactly
what compiled IDs exist to avoid.

What the definition holds matters for what core can answer:

- **Transitions** keep their source state, targets, event descriptors
  (normalized, plus the authored text for export), guard, type and
  document order. SCXML transitions have no author ID, so a label built
  from these is their name.
- **Actions** keep their kind and operands. They do not record where
  they sit: an action belongs to a block (`onentry` or `onexit` of a
  state, a transition, an `<if>` branch, a `<foreach>` body, and so on),
  and a block is a span of the action table.
- **Expressions** are kept as the frontend handed them over
  (`ta_expression`). A chart compiled from SCXML or from a spec with
  source expressions has **source text** (`TA_EXPRESSION_SOURCE`), which
  the compiler copied. A chart built by a host frontend, such as napi's
  JavaScript builder, has **host expressions** (`TA_EXPRESSION_HOST`):
  a retained host value, such as a `napi_ref` to a JavaScript function,
  and no text at all.
- **Source locations** (line and column in a document) are kept for
  nothing.

## Decision

### 1. Three lookups by ID, in the style of `ta_definition_state`

```c
typedef enum {
    TA_TRANSITION_AUTHORED = 0,   /* a <transition> element             */
    TA_TRANSITION_INITIAL,        /* an <initial> or `initial` attribute */
    TA_TRANSITION_BOOTSTRAP       /* the document's initial transition  */
} ta_transition_origin TA_SYMBOLS("TA_TRANSITION_");

typedef struct {
    ta_transition_origin origin;
    ta_state_id source;            /* TA_STATE_NONE for BOOTSTRAP       */
    ta_scxml_transition_kind type; /* EXTERNAL or INTERNAL              */
    /* The event descriptor as authored; empty for an eventless one.     */
    ta_str event;
    TA_COUNTED_BY(target_count) TA_LIFETIME(definition) const ta_state_id *targets;
    uint32_t target_count;
    ta_expr_id condition;          /* TA_EXPR_ID_NONE when unguarded    */
    uint32_t action_count;         /* the size of its content block     */
    uint32_t document_order;
} ta_transition_info;

typedef enum {
    TA_ACTION_IN_ONENTRY = 0,      /* owner: a state                    */
    TA_ACTION_IN_ONEXIT,           /* owner: a state                    */
    TA_ACTION_IN_TRANSITION,       /* owner: a transition ID            */
    TA_ACTION_IN_THEN,             /* owner: an <if> action; `branch`   */
    TA_ACTION_IN_ELSE,             /* owner: an <if> action             */
    TA_ACTION_IN_FOREACH,          /* owner: a <foreach> action         */
    TA_ACTION_IN_FINALIZE,         /* owner: an <invoke>'s state        */
    TA_ACTION_IN_SCRIPT            /* the document's top-level <script> */
} ta_action_block TA_SYMBOLS("TA_ACTION_IN_");

typedef struct {
    ta_scxml_action_kind kind;     /* the element: raise, send, assign, ... */
    ta_action_block block;
    uint32_t owner;                /* a state, transition or action ID, per `block` */
    uint32_t branch;               /* THEN: which <if>/<elseif> branch, from 0 */
    uint32_t index;                /* its place in the block, from 0     */
} ta_action_info;
```

`ta_definition_transition()`, `ta_definition_action()` and
`ta_definition_expression()` each take a params struct with the
definition and the ID, and write a view whose text and arrays are
borrowed from the definition (`TA_LIFETIME(definition)`), as
`ta_definition_state()` does. `ta_definition_expression()` writes the
`ta_expression` as the definition stores it.

- **Every ID a trace record can carry is answerable.** That includes
  `TA_TRANSITION_BOOTSTRAP` and the transitions of `<initial>` elements,
  which is why `ta_transition_info` has an `origin`. If records name
  initial transitions today by an ID outside the authored range, that
  numbering becomes public here and is documented next to
  `TA_TRANSITION_BOOTSTRAP`.
- **Iteration works as it does for states.** `TA_ERR_NOT_FOUND` ends a
  walk over IDs 0, 1, 2, ..., and is also the answer for an unknown or
  stale definition. A retired definition stays readable until it is
  freed (ADR 0043). `ta_definition_inspection` gains `transition_count`,
  `action_count` and `expression_count` beside `state_count`.
- **The element kinds reuse the spec's enums** (`ta_scxml_action_kind`,
  `ta_scxml_transition_kind`), so a host needs no second vocabulary.
  Core's private `ta_action_kind` and `ta_transition_type` stay private.
- **Where an action sits is computed at compile time.** The compiler
  already walks every block, so it records each action's block, owner,
  branch and index in one compact table in the definition's arena. The
  lookup is then a constant-time read. The implementation measures what
  this adds to `definition_bytes` and records it. A definition's size is
  per definition, not per actor, so this does not touch the per-actor
  budget.

### 2. Core returns expressions as stored; the host labels them

Core does not invent text for an expression it has none for. A `SOURCE`
expression comes back with its text. A `HOST` expression comes back with
its host value, which the host that built it knows how to read. napi,
for example, gets back its own reference to the JavaScript function, and
`lib/` may label it with `fn.name` or a shortened `fn.toString()`. That
choice belongs to the host and is not decided here.

### 3. Labels are the host's

Core supplies facts, not display strings. A host builds its own label
from them, for example `idle → busy on order.placed [guarded]` for a
transition, or `<assign> in onentry of busy` for an action. Each host
chooses its format; the OTEL attribute names stay those of ADR 0067.

### 4. Out of scope

- **Source locations.** Recording line and column for each element
  would need the SCXML codec (ADR 0053) and the spec types to carry
  them. That is a separate decision, worth making only if jumping from a
  trace to a line in a document is wanted.
- **Expression ownership** (which element an expression belongs to).
  Trace records already carry a position naming the action, so the
  lookups above answer it.

## Consequences

- A trace sink can name every ID in a record without exporting the
  definition. ADR 0067's open point 4 is closed.
- Transition coverage (ADR 0067, Consequences) can be reported as
  readable transitions, not only IDs.
- napi binds the three functions through the generator (ADR 0013), and
  `lib/tracing.js` uses them to label spans. A columnar trace format can
  ship these facts once per definition instead of once per record.
- The API grows by three functions and three views, and each definition
  grows by one table of action positions.

## Testing

- **Core unit tests:** for a chart with every block kind (`onentry`,
  `onexit`, transition content, `<if>`/`<elseif>`/`<else>`, a
  `<foreach>` body, `<finalize>`, a top-level `<script>`), each action
  reports its kind, block, owner, branch and index. Every transition,
  including the bootstrap and `<initial>` transitions, reports source,
  targets, type, event text and guard. Walks end with
  `TA_ERR_NOT_FOUND` exactly at the counts that inspection reports.
  Stale and retired definitions behave as for `ta_definition_state()`.
- **Agreement with export:** for every chart in the test host's
  catalogue, what the lookups report matches what
  `ta_definition_export()` produces for the same element.
- **Trace naming:** every ID in every record of the `trace.recording`
  scenarios resolves through the lookups.
- **Hosts:** napi returns the function for a host expression and the text
  for a source expression, on Node and Bun.

## Implementation notes

Where the code differs from the decision above:

- **Initial transitions are numbered after the `<transition>` elements.**
  IDs `0 .. n-1` are the `<transition>` elements, and the initial
  transitions of compound states and of the document follow them, so one
  walk covers both. `transition_count` in inspection counts both. Trace
  records never name an initial transition: the bootstrap's `MICROSTEP`
  lists none, and an `<initial>`'s content runs while entering. So the
  numbering is new rather than an existing one made public. The
  interpreter's private marker for the bootstrap microstep was called
  `TA_TRANSITION_BOOTSTRAP`, which clashed with the public origin, and is
  now `TA_ENABLED_BOOTSTRAP`.
- **`ta_transition_info` gains `default_targets`.** A compound state with
  no `<initial>` and no `initial` attribute still has an initial
  transition, to SCXML's default (its first child). The flag says so; the
  export writes nothing for such a transition when it has no content.
- **`branch` became `block_index`.** The spec has no `<elseif>`: the codec
  lowers it to an `<if>` in the `<else>` block, so an `<if>` has exactly
  one THEN block and `branch` would always be 0. What does need an index
  is which of a state's `<onentry>` or `<onexit>` elements an action is
  in (SCXML runs each as its own block), and which `<invoke>` a
  `<finalize>` belongs to. `block_index` carries that, and is 0 for every
  other block.
- **`ta_definition_content()` joined the family.** It landed on `main` at
  the same time (a custom I/O processor reads a literal send target by
  content ID), with two outputs, `media_type` and `out`. It now writes one
  view, `ta_content_info { media_type, bytes }`, like the other lookups,
  and inspection gains `content_count`. Its callers in napi's `lib/` and
  in the test host moved with it.
- **Cost.** The action table is 16 bytes per action in the definition's
  arena. `definition_bytes` counts arena blocks, and for the charts
  measured the table fit in blocks the definition already had:

  | Chart | Actions | `definition_bytes` before | after |
  |---|---|---|---|
  | `traffic_light` | 7 | 4136 | 4136 |
  | `counter_v2` | 3 | 4136 | 4136 |
  | `trace_basic` | 3 | 4136 | 4136 |
  | `oom_raises_jq` | 14 | 4136 | 4136 |
  | `oom_invokes_jq` | 14 | 8272 | 8272 |
  | `oom_phases_jq` | 69 | 9151 | 9151 |

  A definition with many actions pays up to 16 bytes each; nothing is
  per actor.
- **Tests.** `inspection_test` covers a chart with every kind of block, a
  host expression, and unknown, stale and missing-output cases. The export
  round-trip check (`tinyactors-test/ruby/export_roundtrip_check.rb`)
  compares the lookups with the export for all 263 charts of the
  catalogue: each transition as a tuple, each initial transition, and each
  action as the path of its place in the exported tree. A scenario's
  teardown looks up every state, transition, action and expression ID in
  every trace record it recorded, and fails if one does not resolve or if
  a record names an initial transition.
