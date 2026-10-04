# The pre-migration record

These documents describe tinyparse as it was when it was pocketparse, a tool
inside the `ax` repository (ax commit `8fa0c2bf`, `tools/pocketparse/`). They
are kept because they record why the engine has the shape it has. They do not
describe the current code, and nothing here is an instruction.

What changed since they were written is in [`../../HISTORY.md`](../../HISTORY.md).
In short: tinyparse no longer links ax, the parser actor is a four-state
lifecycle in `src/parse.c`, the build is a set of mise tasks, and the `ci/`
scripts these documents name are now `mise run gate:*` and `mise run bench:*`.
The names were rewritten mechanically (`pocketparse` to `tinyparse`, `pp_` to
`tp_`), so a symbol named here may never have existed under that spelling.

| Document | What it was |
|----------|-------------|
| [`ax-execution.md`](ax-execution.md) | The design gate for running the parse as `ax` state machines, one actor per rule. Its verdict was against it. |
| [`ax-topologies.md`](ax-topologies.md) | The survey of six actor topologies that chose "one parser actor runs the VM". The migration replaced that actor with plain C. |
| [`bytecode-vm.md`](bytecode-vm.md) | The analysis that led to the bytecode VM, written under the constraint that it run on `ax`. The VM in `src/vm.c` grew out of it. |
| [`codegen-transform.md`](codegen-transform.md) | The blueprint of the Phase 17 C emitter (`src/emitparser.c`). Its injection section describes `ax` coordination that never shipped. |
| [`build-plan.md`](build-plan.md) | The workstream schedule and finish line pocketparse was built against in ax. |
| [`explore-plan.md`](explore-plan.md) | The plan of the exploration whose results are in [`../peg-vs-glr.md`](../peg-vs-glr.md). |
| [`tinyparse-design.md`](tinyparse-design.md) | The design specification written before the implementation. Its current parts are in [`../explanation/design.md`](../explanation/design.md) and the reference pages; its "Next Steps" section lists surface that was planned and not built. |
| [`lex-goal-implementation.md`](lex-goal-implementation.md) | The finished implementation plan for deferred slots and parametrized lexical goals. Comments in `src/` cite it by name. |

Links in these files were adjusted so they resolve from here. A link that
pointed into ax (its `src/ax/ax-design.md`, `src/axr`, `ci/`) is now plain
text, since those files are not in this repository.
