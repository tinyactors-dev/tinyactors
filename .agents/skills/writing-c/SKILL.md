---
name: writing-c
description: Write or change C in this repository (tinyactors-core's layers and tools, the bindings' C, tinyxml, tinyparse). Covers constants (enums and static const, never #define), output through the existing interfaces (ta_xml_writer, ta_report, ta_output) instead of hand-made writers and markup in string literals, at most three parameters, whole-word names, memory from arenas by lifetime, no fixed buffers that truncate, freestanding layers, and tests the repository owns. Use before writing, porting or reviewing any C function, header or test here.
---

# Writing C in tinyactors

This repository's C has rules a compiler and the lints check, and rules only a
reader checks. Every rule below was broken at least once by an agent who knew
it; each says what to do, and why. Read the project's `AGENTS.md` files (the
root's, then the project's) first: they win where they say more.

## 1. Constants are enums or `static const`, never `#define`

- An integer constant is an enumerator: `enum { CONDITION_SHOWN = 22 };`, or a
  member of a named `typedef enum` when the values form a set
  (`ta_diagram_box_kind`). A public struct still stores it as `uint32_t`.
- Any other constant is a `static const` object of its type:
  `static const double label_row = 320;`, `static const char *const names[] = {...};`.
  `static const char *p` is a mutable pointer: write `*const`.
- A table of constants is a `static const` array of a struct, never parallel
  macros.
- `#define` is for what only the preprocessor can do: include guards,
  `TA_API`-style attributes, conditional compilation. A function-like macro
  is a `static` function (inline when it matters): `take(l, sizeof *p, n)`,
  not `TAKE(l, type, n)`.

Why: a macro has no type, no scope and no name in the debugger, and the
maintainer has asked for enums many times.

## 2. Output goes through an interface, never through ad-hoc writers

Before writing bytes, find the interface that writes that kind of output, and
use it:

| Output | Interface | It does |
|---|---|---|
| XML, SVG, SCXML | `ta_xml_writer` (`<tinyactors/compile.h>`) | escapes text and attributes, checks nesting, keeps the first error |
| a command's answer (Markdown, JSON, XML) | `ta_report` (`src/cli/report.h`) | one tree, three formats |
| bytes that grow | `ta_output` (`src/base/output.h`) over an allocator | grows, reports whether it fit |
| CBOR | `ta_cbor_writer` | streams values |

- No `put`, `put_text`, `print_*` helpers that write markup a byte at a time,
  and no markup or quoted fragments in string literals (`"<rect class=\""`):
  that is a quoting hazard, and a second writer to keep correct.
- Content that is data (a stylesheet, a table of colours, a list of options)
  is a `static const` table of structs, written through the interface in a
  loop: a CSS rule is a selector and its property/value pairs, written as the
  `<style>` element's text by `ta_xml_writer_text`, which escapes it.
- When no interface fits, the missing abstraction is the first thing to build:
  a small, typed writer with its own unit test, used by every caller.
- Thin adapters over an interface are fine when they only convert
  (`number_view` formats a double into a caller's buffer for
  `ta_xml_writer_attribute`); they never write output themselves.

## 3. Signatures: at most three parameters, bytes as `ta_bytes` / `ta_buffer`

Every C function and function-pointer type we write takes at most three
parameters (lint `at-most-three-parameters` on core's public headers and the
bindings' C; the rule holds for static helpers too). Group the rest in a
struct named for a domain concept and its lifetime, built with a designated or
compound literal at the call. Bytes cross as `ta_bytes` in and `ta_buffer *`
or a view whose owner and lifetime are stated, never a pointer and a length.
[Parameter groups](references/parameter-groups.md) lists the groupings (buffer
views, configurations, operations, outcomes); [mechanical
refactoring](references/mechanical-refactoring.md) is how to migrate callers
with ast-grep and the compiler. A signature a platform's typedef fixes
(`napi_callback`, `PyCMethod`, `ta_allocator`'s two) is exempt by that
typedef only.

## 4. Names are whole words

- `ta_` / `TA_` is the only short prefix (`ta_napi_`, `ta_python_` in the
  bindings). Files carry no prefix; internal includes are path-qualified
  (`"base/pool.h"`).
- No retired abbreviation as any `_`-separated word: `len`, `buf`, `ctx`,
  `cfg`, `idx`, `off`, `ch`, `cx`, `cb`, `dm`, `n_states`, ... (the list is
  `tinyactors-core/tools/lint/rules/no-retired-abbreviations.yml`; run
  `mise run lint`). `center_x`, `offset`, `found`, `child_b`.
- Acronyms all capitals or all lowercase (`otlp_writer`, `TA_OTLP_`); the
  root's `acronyms.txt` lists them.

## 5. Memory follows lifetimes

- A freestanding layer (`src/base`, `image`, `core`, `system`, `host`, `cli`,
  `diagram`, ...) uses no C library: no `malloc`, no `string.h`, no `stdio`,
  no recursion (walk trees with an explicit stack or in document order). The C
  library is used only in the topmost platform file (`tools/tinyactors/platform.c`).
- Memory comes from an arena (`ta_growable_arena`) whose life is the
  operation's: one invocation, one call (a mark taken at entry and released
  at exit), one parse. Allocate arrays sized up front from counts; free once.
- No fixed-size buffer that silently truncates (`char data[64 * ...]` and a
  loop that stops when it is full). Grow through the arena (`ta_output` over
  `ta_growable_arena_allocator`), or refuse with a status.
- Every allocation that can fail is checked, and a failure is a status
  (`TA_ERROR_NO_MEMORY`), never a crash; a test drives the arena to fail at
  every size until it succeeds.
- Floating point is allowed in a freestanding layer, but no `libm`: write
  `ceil_of`, an integer square root, a squared distance where only the
  nearest is asked.

## 6. Tests are the repository's own

- Every building block gets a unit test in `tests/unit/` (core) that runs
  under the sanitizer build: properties (invariants, determinism), refusals,
  running out of memory, and the escaping of every output.
- A port of another implementation's algorithm keeps that algorithm's step
  comments, so the two read side by side, and is checked against the
  original once, over every input the repository has. Its expected results
  then become fixtures **the repository owns** (a corpus of inputs and their
  expected outputs under `tests/`), with their provenance written once. No
  test, task or build depends on the original implementation afterwards; a
  deliberate change to the algorithm regenerates the fixtures with a task and
  the diff is reviewed.
- A test fixture is a `static const char name[]`, not a macro; fixture files
  in a shared scratch directory carry names no other test uses.

## 7. Before calling it done

- `mise run build` (every warning is an error: `-Wconversion`,
  `-Wfloat-conversion`, `-Wshadow`, `-Wcast-align`; compare doubles with
  `!= 0`, cast `void *` through `(void *)` to raise alignment).
- `mise run lint` in the project, `mise run check-acronyms` at the root.
- The project's `mise run test`, and the projects the root `AGENTS.md`'s
  table says the change reaches.
- Grep the change for `#define`, string literals holding `<` or `"`, fixed
  arrays sized by a guess, and helpers named `put*`: each is a rule above.
