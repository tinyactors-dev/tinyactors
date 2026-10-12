# Decisions

The design decisions the guide (`tinyactors-core/docs/bindings.md`) and
the headers leave open, each with its evidence: a scenario in this
design, and `path:line`, a primary source or a spike
(`.agents/skills/reasoned-analysis`). The commit that records one is the
commit that acts on it.

## Per-environment state is the environment's instance data

**Decision.** The addon's state for one environment (the main thread, or
a worker) is a `ta_napi_module` set with `napi_set_instance_data` at
module init and freed by its finalizer (`src/module.c`); nothing is a C
global (the lint rule `no-mutable-globals`).

**Scenario.** An application runs a host in the main thread and another
in a `Worker`, each loading the addon: a `napi_ref` (the
`TinyactorsError` class, a host's handlers) made in one environment
means nothing in the other, so a global holding one breaks the second
environment.

**Evidence.** A spike (2026-10-12, node v22.23.2 and bun 1.4.1, an addon
built with the pinned clang 22 and `-bundle -undefined dynamic_lookup`):
`napi_set_instance_data` returned `napi_ok` in both runtimes; a counter
in the instance data counted 1, 2 in the main thread, 1 in a worker that
loaded the addon, then 3 in the main thread: each environment has its
own. `napi_type_tag_object` and `napi_check_object_type_tag` told a
tagged object from a plain one in both. `test/load.test.mjs` keeps the
worker half of it.
