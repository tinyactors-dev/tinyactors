# ADR 0057: Core ships the null data model

**Status:** Proposed; applies ADR 0005 and ADR 0023
**Date:** 2026-09-23

## Context

ADR 0005 makes data models pluggable, and core ships none. Every host that
compiles a chart without data must still supply a `ta_datamodel`. The null
data model (SCXML B.1) is that minimum, and it is not host-specific. It
stores nothing, allocates nothing per actor, and has one expression: the
`In(state)` predicate in a `cond`.

There are now two copies, and they disagree:

- `tinyactors-test/src/null_datamodel.c` (271 lines) implements B.1,
  including `In()`, through `ta_dm_host_find_state()` and
  `ta_dm_host_in_state()`.
- `tinyactors-napi/src/null_datamodel.c` (131 lines) rejects every
  expression. A chart that uses `cond="In('s')"` fails to compile under
  Node and works under the test host. It is also a second place where
  `"null"`, fault codes and messages are defined.

Every future binding (CPython, CRuby, WASM, embedded; ADR 0024) would write a
third copy. Nothing in the null profile depends on the host. It only reads
compiled expressions and asks core about the configuration, which core
answers anyway.

## Decision

1. **`ta_datamodel ta_null_datamodel(void)`** is a public function in
   `include/tinyactors.h`. It returns a stateless adapter with `.self = NULL`
   and a static vtable, named `"null"`.
2. **It implements SCXML B.1 exactly.**
   - `eval_condition` accepts `In(id)` (with the ID quoted or bare, as the
     test host accepts today). It answers from the configuration. An ID
     the chart does not contain is `false`, not an error.
   - Every other operation fails with a fault of `TA_ERR_UNSUPPORTED`, which
     the interpreter turns into `error.execution`.
   - `prepare_definition` validates expressions at compile time, so a
     non-`In()` expression fails when the chart is defined, not on first use.
3. **It follows the core C rules.** It uses no libc (ADR 0023) and has no
   per-actor instance (instance size 0). The only per-definition state is
   the expression table that `prepare_definition` already receives.
4. **Hosts delete their copies.** `tinyactors-test` and `tinyactors-napi`
   call `ta_null_datamodel()`. The shared behaviour is tested once in
   core's suite, and each binding keeps a smoke test.

## Consequences

- A chart with no data behaves the same in every binding, and `In()` works
  under Node.
- Core ships one data model, but it is still not tied to one: ADR 0005 is
  unchanged, and richer models (JSON, ECMAScript, JavaScript values) stay
  host adapters.
- The null profile's error messages become part of core's behaviour. Tests
  in hosts that match on the old texts must be updated.
