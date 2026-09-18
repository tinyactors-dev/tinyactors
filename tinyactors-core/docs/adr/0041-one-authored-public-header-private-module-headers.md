# ADR 0041: One authored public header; private module headers

**Status:** Accepted  
**Date:** 2026-09-18

## Context

`include/tinyactors.h` used to be an index that included every `src/*.h`
module header. Those headers mixed public declarations with implementation
layouts (`struct ta_system`, `ta_actor`, continuation state) and internal
helpers (`*_internal` functions, atom and timer tables). Everything reachable
from the index was therefore de facto API, and `extern "C"` blocks were the
only, unreliable, marker of intent. Bindings (ADR 0024) and generated test-host
bindings need an exact, machine-readable public surface.

SQLite solves the same problem with one hand-maintained public header
(`sqlite3.h`), private headers that include it (`sqliteInt.h`), opaque handle
types, and an amalgamation that gives private functions internal linkage.

## Decision

- `include/tinyactors.h` is authored by hand and is the complete public API.
  Every public function is declared there, and only there, with `TA_API`.
  Handles whose layout is private (`ta_system`, `ta_definition`,
  `ta_session_image`, `ta_dm_host`, `ta_arena_block`) are opaque typedefs.
- `src/*.h` are private module headers. They include `tinyactors.h` (through
  `types.h`) and add layouts and internal functions. `src/internal.h` lists them
  in dependency order.
- `mise run amalgamate` emits `include/tinyactors.c`: the public header, then
  the private headers, then the module sources. Definitions of public functions
  are marked `TA_API`; every other defined function is marked `TA_PRIVATE`
  (`static` by default). `--check` fails when a public prototype lacks `TA_API`
  or a private header redeclares a public function.
- Data-model adapters never see `struct ta_definition`. `prepare_definition()`
  receives the expression table directly (`expressions`, `expression_count`),
  borrowed until `destroy_definition()`, in line with ADR 0031.

## Consequences

The public surface is one reviewable file, and an API change is a diff to it.
Tools such as binding generators parse `tinyactors.h` alone. Shipping
`tinyactors.h` plus `tinyactors.c` exports exactly the `TA_API` symbols.

Internal layouts can change without affecting embedders, so inspection cannot
hand out pointers to them. It reports plain facts in `ta_actor_inspection` and
variable-length state (active configuration, history, delayed sends) through
list functions that fill caller buffers; `ta_definition_state()` resolves
state IDs to the author's names.

White-box tests that need private layouts include `src/` headers explicitly
and say so.
