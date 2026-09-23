# ADR 0058: A compile error says what is wrong and where in the chart

**Status:** Accepted; extends ADR 0042, completes ADR 0053 §4
**Date:** 2026-09-23

## Context

`ta_definition_compile_spec()` answers every problem in a chart with a bare
`ta_result`. About forty distinct checks in `src/builder.c` all return
`TA_ERR_INVALID` or `TA_ERR_UNSUPPORTED`, with nothing that says which check
failed or which part of the spec it was about. Some of them, cited as of
`main` at 1afb509, before this ADR:

| Check | Where |
|---|---|
| A state ID, parameter name, data ID, `<foreach item>` or literal `sendid` is empty | `ta_compiler_atom`, `src/builder.c:69` |
| Two states share an ID | `src/builder.c:548` |
| A transition or initial target names no state | `ta_compiler_resolve`, `src/builder.c:584` |
| An `<initial>` target is not a proper descendant of its owner | `src/builder.c:618` |
| An event descriptor has an empty segment (`a..b`) | `src/builder.c:658` |
| A literal `delay` is not an SCXML duration | `ta_duration_parse`, `src/builder.c:75` |
| `<send>`, `<donedata>` or `<invoke>` combine members SCXML makes exclusive | `src/builder.c:354`, `:802`, `:888` |
| An atomic or final state has children or an `<initial>`; a compound or parallel state has none; a parallel state has an `<initial>` | `src/builder.c:928`, `:934` |
| A history state is at the root, has children, or its parent is not compound or parallel | `src/builder.c:940` |
| A history state's default transition is not exactly one targetless-event, conditionless transition into its parent | `src/builder.c:959`–`:969` |
| `<donedata>` outside a final state; `<invoke>` inside a history state | `src/builder.c:922`, `:998` |
| The chart has no states; neither `name` nor `identity` gives it a family | `src/builder.c:1139`, `:1060` |
| The chart's `datamodel` is not the adapter's name | `src/builder.c:1189` |
| The chart has more states than a configuration can hold | `src/builder.c:1115` |
| The data model's `prepare_definition()` refuses the expression table | `src/builder.c:1265` |

When a chart was written by hand as a C compound literal, the author can
bisect it. When it came from a file, nobody can: the person who wrote
`<transition target="idel">` gets `TA_ERR_INVALID`, and the file has no line
to point at.

ADR 0053 made this worse by design. It lets a codec's spec point into the
input bytes, "which is what lets an embedder map a compiled expression back
to the construct it came from". But the compiler never reports a construct,
so there is nothing to map. And the `ta_definition_import()` that ADR 0053
§4 promises would report decode errors with a line and column, and the more
common semantic errors with none.

Two facts shape the design:

1. **Core sees a spec, not a file.** It cannot report a line number. It
   can report *where in the spec*: a path from the root. It can also report
   *which authored text* is at fault, as the exact `ta_str` it was given.
2. **That text often lies inside the input bytes.** A codec over a syntax
   tree keeps state IDs, targets, event descriptors and expressions as
   slices of the source (ADR 0053). If the subject of an error is such a
   slice, its byte offset is `subject.ptr - input.ptr`. The position comes
   from pointer arithmetic and needs no help from the codec.

## Decision

### 1. `ta_definition_compile_spec()` can describe its failure

```c
/*
 * One step from a spec's root toward the element an error is about. `index`
 * selects within a list member and is TA_SPEC_INDEX_NONE for a single one.
 */
typedef enum {
    TA_SPEC_MEMBER_NONE = 0,
    /* Lists. */
    TA_SPEC_MEMBER_STATES, TA_SPEC_MEMBER_TRANSITIONS, TA_SPEC_MEMBER_TARGETS,
    TA_SPEC_MEMBER_ONENTRY, TA_SPEC_MEMBER_ONEXIT, TA_SPEC_MEMBER_ACTIONS,
    TA_SPEC_MEMBER_THEN, TA_SPEC_MEMBER_ELSE, TA_SPEC_MEMBER_BODY,
    TA_SPEC_MEMBER_FINALIZE, TA_SPEC_MEMBER_INVOKES, TA_SPEC_MEMBER_DATA,
    TA_SPEC_MEMBER_PARAMS,
    /* Single members. */
    TA_SPEC_MEMBER_INITIAL, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_MEMBER_SCRIPT,
    TA_SPEC_MEMBER_ID, TA_SPEC_MEMBER_NAME, TA_SPEC_MEMBER_EVENT,
    TA_SPEC_MEMBER_CONDITION, TA_SPEC_MEMBER_TYPE, TA_SPEC_MEMBER_SRC,
    TA_SPEC_MEMBER_TARGET, TA_SPEC_MEMBER_DELAY, TA_SPEC_MEMBER_DATA_VALUE,
    TA_SPEC_MEMBER_LOCATION, TA_SPEC_MEMBER_EXPR, TA_SPEC_MEMBER_ITEM,
    TA_SPEC_MEMBER_INDEX, TA_SPEC_MEMBER_SENDID, TA_SPEC_MEMBER_DATAMODEL,
    TA_SPEC_MEMBER_IDENTITY
} ta_spec_member TA_SYMBOLS("TA_SPEC_MEMBER_");

#define TA_SPEC_INDEX_NONE UINT32_MAX
typedef struct { ta_spec_member member; uint32_t index; } ta_spec_step;

typedef enum {
    TA_COMPILE_ERROR_NONE = 0,
    TA_COMPILE_ERROR_MALFORMED_SPEC,        /* a tag out of range; a required pointer absent */
    TA_COMPILE_ERROR_MISSING_VALUE,         /* required text or expression absent or empty */
    TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS,     /* params with content; src with content */
    TA_COMPILE_ERROR_DUPLICATE_STATE,
    TA_COMPILE_ERROR_UNKNOWN_STATE,         /* a target or initial names no state */
    TA_COMPILE_ERROR_TARGET_OUTSIDE,        /* an initial or history target outside its owner */
    TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR,
    TA_COMPILE_ERROR_BAD_DURATION,
    TA_COMPILE_ERROR_BAD_STRUCTURE,         /* children, <initial> or <donedata> where the kind forbids */
    TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
    TA_COMPILE_ERROR_NO_STATES,
    TA_COMPILE_ERROR_NO_FAMILY,
    TA_COMPILE_ERROR_DATAMODEL_MISMATCH,
    TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT, /* TA_SCXML_ACTION_INVOKE */
    TA_COMPILE_ERROR_LIMIT,                 /* a bound core enforces was reached */
    TA_COMPILE_ERROR_HOST_EXPRESSION,       /* a host expression's retain() failed */
    TA_COMPILE_ERROR_EXPRESSION_REJECTED    /* the data model refused an expression (§3) */
} ta_compile_error_code TA_SYMBOLS("TA_COMPILE_ERROR_");

typedef struct {
    ta_compile_error_code code;
    /* From the spec's root to the element at fault; in `error_arena`. Empty
     * when no arena was given or the error is about the whole chart. */
    TA_COUNTED_BY(path_length) TA_OPTIONAL const ta_spec_step *path;
    uint32_t path_length;
    /* The authored text at fault, borrowed from the spec exactly as given:
     * the unknown target, the duplicate ID, the bad duration, the refused
     * expression's source. Empty when the error is not about text. */
    ta_str subject;
    /* Within `subject`, the bytes at fault, when narrower than all of it: the
     * empty segment of a descriptor, or the offset a data model reported. */
    uint32_t subject_offset;
    uint32_t subject_length;
    /* A second place the error involves, borrowed from the spec: the first
     * state with a duplicate ID, the owner a target must lie within, or the
     * state ID nearest to an unknown target. Empty when there is none. */
    ta_str related;
    /* Why, in words: core's fixed phrase, or the data model's message copied
     * into `error_arena`. */
    ta_str message;
} ta_compile_error;

typedef struct {
    TA_IN const ta_scxml_spec *spec;
    ta_datamodel datamodel;
    ta_allocator allocator;
    ta_definition **out;
    /* New. Filled when the result is TA_ERR_INVALID or TA_ERR_UNSUPPORTED
     * because of the spec. Argument errors and TA_ERR_NOMEM leave `code` at
     * TA_COMPILE_ERROR_NONE. */
    TA_OUT TA_OPTIONAL TA_LIFETIME(input) ta_compile_error *error;
    /* New. Holds the path and any copied message. Without it the error still
     * carries code, subject, related and core's own phrase, but no path and
     * no data-model message. */
    TA_OPTIONAL TA_ARENA ta_arena *error_arena;
} ta_definition_compile_params;
```

The result codes do not change. A caller that ignores `error` sees exactly
the behaviour it sees today.

**`subject` and `related` are borrowed, never copied.** This is the point of
the design. Their pointers are the spec's own pointers, so when the spec came
from a codec that slices its input, they point into that input and give the
byte offset of the fault. A copy would lose that.

**`related` is the context a person needs next.** The duplicate ID shows
where the first definition was. The descendant rule shows which state the
target had to be within. For an unknown target, `related` is the existing
state ID with the smallest edit distance (a transposition counts as one
edit), when that distance is at most a third of the subject's length. `target="idel"` then reports `idle` as the
nearest. Finding it costs one pass over the state table, and only on the
error path.

### 2. The first error in compile order, and nothing on the success path

The compiler reports one error: the first it meets. The walk order is fixed
and documented: the chart's `datamodel` requirement; every state in
document order to check its kind and collect its ID; the root's `<data>`;
for each state in document order, its structural checks, transitions, the
history default (for a history state), `<initial>`, `<onentry>`,
`<onexit>`, `<data>`, `<donedata>` and `<invoke>`; the root `<initial>`;
the root `<script>`; the configuration limit and the family; then the data
model. The same chart therefore always reports the same error.

The compiler keeps the current path as a stack of `ta_spec_step`s. It pushes
a step when it descends and pops one when it returns. The stack is a `ta_vec`
that the compiler already knows how to grow. When compilation fails and an
`error_arena` was given, the stack is copied into that arena. On success
nothing is copied, and the arena is not touched.

Collecting every error was considered and rejected for now. Pass two depends
on pass one (a duplicate ID makes every target resolution ambiguous), so the
errors after the first are mostly consequences of it. A codec's decode also
stops at its first error (ADR 0053), and one error per stage keeps
`ta_definition_import()` simple. If an editor integration later needs more,
an error list can be added beside this without changing `ta_compile_error`.

### 3. A data model can say which expression it refused

`prepare_definition()` sees the whole expression table and may already reject
it: the ECMAScript test model refuses host expressions there. A model that
parses early could name the expression that fails to parse, but today it has
no way to say which one.

```c
#define TA_EXPR_ID_NONE ((ta_expr_id)UINT32_MAX)  /* new */

typedef struct {
    ta_expr_id expression;  /* TA_EXPR_ID_NONE when not about one */
    uint32_t offset;        /* within that expression's source */
    uint32_t length;
    ta_str message;         /* the adapter's until core next calls it */
} ta_expression_rejection;

typedef struct {
    TA_COUNTED_BY(expression_count) TA_OPTIONAL TA_LIFETIME(definition) const ta_expression *expressions;
    uint32_t expression_count;
    ta_allocator allocator;
    void **shared;
    /* New. Set by an adapter that returns TA_ERR_INVALID or TA_ERR_UNSUPPORTED
     * because of one expression. Absent when the caller wants no detail. */
    TA_OUT TA_OPTIONAL TA_LIFETIME(call) ta_expression_rejection *rejected;
} ta_datamodel_prepare_definition_params;
```

Core copies `message` into `error_arena` before it calls the adapter again,
so an adapter may point it at a buffer of its own that it reuses. Core turns the rejection into
`TA_COMPILE_ERROR_EXPRESSION_REJECTED`, with the expression's source as
`subject` and `offset`/`length` as the narrowed range.

To get the path, core does not store one per expression. It **replays the
walk**: the compiler assigns expression IDs in walk order, so walking the
spec again and stopping at the N-th expression gives its path. The replay
runs only when an adapter names an expression.

This ADR does not require any data model to parse early. SCXML reports a bad
expression as `error.execution` when it is evaluated, and a model that waits
until then stays conformant. The field only lets a model that parses early
say where the problem is.

### 4. Rendering

```c
/*
 * Writes `error` as one line into `arena`:
 *   states[2](idle).transitions[0].targets[0]: unknown state "idel"; nearest is "idle"
 * A state step shows its ID so the line can be read without the spec.
 */
typedef struct {
    TA_IN TA_OPTIONAL const ta_scxml_spec *spec;  /* null: no state IDs shown */
    TA_IN const ta_compile_error *error;
    TA_ARENA ta_arena *arena;
    TA_OUT TA_LIFETIME(arena) ta_str *out;
} ta_compile_error_format_params;
TA_API ta_result ta_compile_error_format(TA_IN const ta_compile_error_format_params *params);
```

This format is for people and has no stable grammar. Tools use the struct.

**The line has a fixed maximum length.** It is never longer than
`TA_COMPILE_ERROR_TEXT_MAX` (1024) bytes, however deep the path or long the
text. Neither the spec's depth nor the length of its text can make it grow:

- A path of more than 8 steps shows its first 2 and last 6 steps, and says
  how many steps it left out. The end of the path is kept because it
  locates the fault.
- Each state ID, subject and related text is cut to 64 bytes, and the
  message to 96. A cut falls on a UTF-8 character boundary and ends in
  `...`.
- Control characters, quotes and backslashes are escaped, so the line stays
  one line.

A typo at depth 500, where every ID is 20,000 bytes, formats as a line of
534 bytes:

    states[0](level0-xxx…...).states[0](level1-xxx…...).[494 steps].states[0](level496-…...)…
      .transitions[0].targets[0]: unknown state "idel"; nearest is "idle"

The struct keeps the whole path, because tools may need every step. The
path is linear in the depth of the spec the caller built. The data-model
message is the one thing core copies, so it is also bounded: core keeps at
most `TA_COMPILE_ERROR_MESSAGE_MAX` (256) bytes of it. The nearest-name
search skips candidates whose difference in length already rules them out,
and is not run for unknown names longer than 128 bytes. That bounds the
error path's work too.

### 5. Through `ta_definition_import()`, a compile error gets a position

ADR 0053 §4 defines `ta_definition_import()`. On a compile failure it fills
the compile error and derives a `ta_chart_position` in `input`, trying these
in order:

1. `subject` lies within `input`: the position of
   `subject.ptr + subject_offset`.
2. Otherwise, the nearest step up the path whose element has authored text
   within `input`. For a state this is its `id`, for a transition its
   `event`, for an action its first text or expression. That text's
   position is used.
3. Otherwise, no position. `located` is false.

Line and column come from counting newlines in `input` up to the offset, on
the error path only. For a codec that slices, this makes semantic errors as
precise as syntax errors. For a codec that copies, the path, subject and
message remain, which is what a C author gets today plus the path.

## Consequences

- `<transition target="idel">` in a file reports its line and column, the
  word `idel`, and `idle` as the nearest state. Today it reports
  `TA_ERR_INVALID`.
- The C compound-literal front end, the Ruby DSL and the JSON mirror all get
  a path and a subject with no work of their own. A front end that keeps its
  own source map (the Ruby DSL, from spec element to Ruby line) can resolve
  the path against that map.
- A successful compile does the same work as before plus pushing and popping
  a path stack. The edit-distance search, the expression replay and the
  line/column count run only on failure.
- `ta_compile_error` borrows from the spec, so it is valid only while the spec
  is. For `ta_definition_import()`, that means until the caller resets the
  decode arena. This matches the rest of ADR 0053.
- `ta_datamodel_prepare_definition_params` gains a member. Core builds those
  params, so an adapter that never writes `rejected` behaves as before.
- `ta_spec_member` has to follow `ta_scxml_spec`. Adding a spec member also
  means adding its step. Nothing checks this today, so a test compiles a
  chart with a fault under every list member and asserts the reported path.
- The ordering in §2 becomes a contract. Reordering the compiler's checks
  changes which error a chart with several faults reports. That is allowed,
  but it is a visible change and needs its own note.

## Implementation notes

Recorded when this ADR was implemented (2026-09-23).

- **Edit distance counts a transposition as one edit** (optimal string
  alignment). Plain Levenshtein puts `idel` two edits from `idle`, over a
  third of four, so the ADR's own example would not have suggested
  anything.
- **The compile order in §2 was corrected to the code.** The code checks
  the data-model requirement first, a history state's default transition
  right after that state's transitions, and the configuration limit and
  family after the root `<script>`. The draft left these out. The code was
  right; the text now says what it does.
- **A rejection's message is the adapter's until core next calls it**, not
  "for the call". Core can only read it after `prepare_definition()`
  returns, so a buffer on the adapter's stack would already be gone. Core
  copies it before the replay, and the replay does not call the adapter.
- **Lifetime annotations.** The API vocabulary has no `spec` scope, so the
  compile error is `TA_LIFETIME(input)`, meaning the spec. `rejected` is
  `TA_LIFETIME(call)`; a `ta_str` member cannot carry one.
- **The replay compiles again**, onto a scratch definition, until the
  walk reaches the named expression. It interns nothing new (every atom
  was interned by the first compile) and never prepares the scratch
  definition. A rejection that names no expression, or one past the table,
  has an empty path.
- **A refusal from below the compiler's own checks** is reported as
  `TA_COMPILE_ERROR_MALFORMED_SPEC` with an empty path, so a refused spec
  never has `TA_COMPILE_ERROR_NONE`. None is known to happen today.
- **Anchors for §5 step 2.** A state's `id`; a transition's `event`, then
  its `cond`, then its first target; an action's first text member (event,
  location, expression, condition, sendid, target, type, id); an
  `<invoke>`'s `id`, `type` or `src`; a `<data>` or `<param>`'s name; an
  `<initial>`'s first target.
- **Duplicate IDs are found by sorting, not by comparing every pair.** After
  pass one, the compiler builds one index of `(name, state)` keys and sorts
  it in place: insertion sort up to 16 keys, heapsort above. It reports the
  duplicate whose second occurrence comes first in document order, with the
  first occurrence as `related`. If pass one stopped at another fault, any
  duplicate among the states collected so far comes earlier in document
  order, so it is reported instead. The first-error rule of §2 therefore
  holds exactly as it did with a per-state check. Target resolution uses the
  same index: a scan up to 16 states, a binary search above. Both pass one
  and resolution were quadratic before.
- **Every code is reached by a test, `TA_COMPILE_ERROR_LIMIT` included.** A
  chart of 524,225 states, one more than 8191 configuration words can hold,
  compiles to that error in under a second.
- **The test host prints located errors.** The scenario runner, the IRP
  check and the export round-trip check write an import failure as
  `line:column: <formatted error>`, using this function over the RPC.

## Sources

In-tree, cited above by file and line as of 1afb509: `src/builder.c:69`,
`:75`, `:354`, `:548`, `:584`, `:618`, `:658`, `:802`, `:888`,
`:922`–`:969`, `:998`, `:1060`, `:1115`, `:1139`, `:1189`, `:1265`;
`tinyactors-test/src/ecmascript_datamodel.c:820`.
