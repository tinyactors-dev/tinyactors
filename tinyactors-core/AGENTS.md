# C coding rules

- never use malloc/free, stdlib or other implementation-providing C headers
- stdint, stdefs, and stdbool are okay
- functions are allowed a maximum of three parameters; introduce struct types if you need more.
- the public API is `include/tinyactors.h` only: declare every public function there with `TA_API`; `src/*.h` headers are private.
- run `mise run amalgamate` after changing any header or source; never edit `include/tinyactors.c` by hand.
- run `mise run api` after changing `include/tinyactors.h`; annotate new declarations with the `TA_*` macros it asks for (see `docs/internal/api-ir.md`), and run `mise run api-test` after changing the extractor in `mise/lib/api`.

## No recursion in core (ADR 0060)

No function in `src/` may call itself, directly or through other functions.
The call graph of core is acyclic. The reason: nesting depth in a chart, an
event name or a pattern is chosen by whoever builds the input, and core
runs on hosts whose stack is small and fixed (an MCU task, a WASM stack, a
host thread). Recursion turns input depth into C-stack depth, and C-stack
overflow is a crash that no `ta_result` can report.

What to do instead, by kind of walk:

- **Walks over nested input or nested compiled data** (a `ta_scxml_spec`,
  nested action blocks, a definition during export): use an explicit stack
  of frames in a `ta_vec` or an arena. A frame holds the list being walked,
  the index into it, and a `stage` field when the walk does work before,
  between or after the children, as `<if>` (condition, then block, else
  block, store) and `<foreach>` (array, item, index, body) do.
- **Where that stack lives:**
  - the compiler: a `ta_vec` in `ta_compiler`, using the compile allocator.
    It is also the ADR 0058 error path: every frame names its list and
    index, so the frames are the path.
  - export: the exporter's arena.
  - running executable content: `ta_exec_frame` frames in the
    interpreter's scratch, taken when the interpreter starts
    (`ta_interpreter_init`), sized from the definition's recorded maximum
    action nesting depth. Interpreters nest (cancelling an invocation exits
    the child inside the parent's interpreter), and each has its own
    scratch, so there is no one system-wide stack. Actors carry no frames,
    except while paused inside a microstep (ADR 0061): then the position,
    the sets and the frames move into one `ta_continuation` the actor owns,
    and back into scratch when it resumes. That move happens between
    units of executable content, never while an action runs, and never
    allocates: the continuation is reserved before the run starts
    (ADR 0070).
    **Never grow a frame stack while actions run.** A growth that fails after an
    `<assign>` has taken effect breaks ADR 0047 and ADR 0070: a step that
    runs out of memory must have had no effect that doing it again would
    repeat.
    Scratch space for state-tree queries is reserved the same way, sized
    from the definition's recorded maximum state depth.
- **Queries over the state tree at run time**: use the compiled layout
  instead of a stack. States are numbered in document pre-order, so a
  state's subtree is a contiguous ID range, and every child has a higher ID
  than its parent. A bottom-up question (is this compound or parallel state
  in a final state?) is one pass in reverse ID order. The entry set is
  order-sensitive, so it runs as continuation frames, one per activation of
  the recursive SCXML algorithm, which keep its order exactly. History
  defaults are a worklist bounded by the number of history states.
- **Matchers** (event descriptors against event names): iterative, with
  polynomial worst-case time. No backtracking recursion. A `*` segment may
  appear anywhere in a descriptor and matches zero or more segments; match
  with the two-pointer wildcard algorithm (remember the last `*` and the
  event position it resumed from), which is O(event × pattern) and uses
  O(1) space.
- **Visit order is part of the contract.** Replacing a recursion must not
  change the order in which the compiler visits the spec. The revision hash,
  the numbering of states and expressions, the layout of nested action
  blocks, and the ADR 0058 replay all depend on it. The export round-trip
  check (every chart keeps its revision) must still pass unchanged.
- **Depth is not limited for safety.** With no recursion, deep input costs
  heap memory proportional to its depth, not stack. Do not add a nesting
  limit to core to protect the stack; bounding untrusted input is the
  front end's job (the SCXML codec's `max_depth`).

`mise run check-recursion` builds core's call graph and fails on any cycle.
It is part of `mise run test`.

## No allocator call in a running macrostep (ADR 0102)

A macrostep never calls core's allocator, to allocate or to free.
`mise run check-macrostep-alloc` (part of `mise run test`) walks core's
call graph from the functions a macrostep runs in and fails on every
allocator call it reaches (`mise/ast-grep/rules/core-allocator-call.yml`
at the repository's root says what counts). Allocate at a safe point
instead: the take, a refill between inputs, registration, spawn or load.
The calls older than the check are on the task's allow-list, which only
shrinks: when a change removes one, take its entry off; never add one.

## ADRs are an append-only record

`docs/adr/` is the history of the design. A reader must be able to go
through the ADRs in order and see what each decision changed and what it
cost, so an accepted ADR is not rewritten to match what came later.

- **A change of mind is a new ADR.** Do not edit the Context, Decision or
  Consequences of an accepted ADR, even to "correct" it after a later
  decision.
- **The only edits to an earlier ADR are on its Status line:** marking it
  implemented, and pointers to later ADRs ("refined by ADR 0067",
  "superseded by ADR 0038"), with a few words on what changed.
- **Implementation notes belong to the change that implements that ADR.**
  They record where the code differs from the text. A later ADR does not
  revise them.
- **Each ADR records its own costs** with before and after figures: bytes
  per actor, per event and per pending send, allocations, image format
  changes. When a later change moves a figure, its own ADR states the new
  figure and its cause; the earlier ADR's figure stays as it was.
