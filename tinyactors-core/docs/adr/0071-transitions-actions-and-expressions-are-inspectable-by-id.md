# ADR 0071: Transitions, actions and expressions are inspectable by ID

**Status:** Accepted; not yet implemented. Refines ADR 0042 and ADR 0049; serves ADR 0067 (closes its open point 4).
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
