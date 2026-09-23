# ADR 0057: Core ships the null data model

**Status:** Accepted and implemented; applies ADR 0005 and ADR 0023. See the implementation notes for where the code differs from the text.
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

## Implementation notes

Recorded when this ADR was implemented (2026-09-23).

- **Where the two copies disagreed, the ADR won.** The test host accepted
  every source expression when a chart was defined and faulted a
  non-`In()` one only when it ran. napi refused every expression, `In()`
  included. Core accepts `In()` and refuses everything else in
  `prepare_definition`, as D2 says. The test host's scenarios that
  expected run-time `error.execution` for assign, script, foreach, send
  data, send params and a non-`In()` condition now expect the chart to be
  refused when it is compiled.
- **The refusal names the expression** through ADR 0058's
  `ta_expression_rejection`. A source expression that is not `In(id)` is
  `TA_ERR_INVALID` with `TA_COMPILE_ERROR_EXPRESSION_REJECTED`, its path,
  its source as `subject`, and the message "the null data model has only
  the In(state) predicate". A host-handle expression is
  `TA_ERR_UNSUPPORTED`.
- **What still fails at run time.** `prepare_definition` sees the
  expression table, not where each expression is used, so it cannot refuse
  `In()` used as a value (`<log expr="In('a')">`). That compiles and faults
  as `error.execution` when it runs. Literal `<content>` and a `<data>`
  without an initializer carry no expression, and also fault at run time.
- **Per-definition state.** A chart with no expressions keeps nothing
  (`shared` is null). A chart with expressions keeps one record: a pointer
  to the expression table core already owns, and its length.
- **Error messages** are the test host's texts, plus the new refusal
  message above.
- **D4 is done for `tinyactors-test` only.** Its copy is deleted, and its
  `datamodel.null` adapter and scale test call `ta_null_datamodel()`.
  `tinyactors-napi/src/null_datamodel.c` remains; napi is separate work.
- **Core tests** that stubbed a model named "null" (`codec_test`,
  `delayed_send_test`, `event_pattern_test`) use `ta_null_datamodel()`.
  Stubs that count calls or refuse things on purpose (`deep_test`,
  `compile_error_test`, `export_test`) stay.
- **The RPC does not return `compile_spec`'s error.** The `error` member of
  `ta_definition_compile_params` has no `TA_VALID_ON` annotation, so the
  generated binding encodes it as null on a refusal. The scenario that
  checks the refusal's detail therefore imports an SCXML document, whose
  `ta_import_error` is annotated. This is an ADR 0058 annotation gap, not
  part of this ADR.
