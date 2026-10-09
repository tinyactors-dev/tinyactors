# AGENTS.md

Portable, embeddable SCXML interpreter + cooperative actor-system VM in C.
Read `docs/architecture.md` before changing runtime code; `docs/layout.md`
lists where things go.

## Commands (all via mise)

The repository root's AGENTS.md applies too: tasks are Ruby file tasks in
`mise/tasks/`, and the build goes through the root's pinned toolchain
(`mise/lib/toolchain/`). `mise/core.rb` lists the layers, the test
programs and the builds (`dev`: ASan/UBSan at -O1; `release`; `addon`:
position-independent; `wasi`).

- `mise install` (at the root) installs the pinned toolchain.
- `mise run build` builds every layer, test program and tool
  (`build/dev/lib`, `build/dev/bin`); `--release` builds `build/release`.
- `mise run test` builds and runs every test program, then `lint`,
  `check:freestanding`, `api --check` (after `amalgamate`), `wasm:test`, `w3c`, `node:test`, `web:test`, `surface`,
  `conformance:js`, `w3c:js`, `examples:test` and `check:wasm`; `--full`
  adds `torture:test` and `check:cross`. Each also runs on its own.
- `mise run check:freestanding` must pass for any change under
  `src/{base,image,core,system}`; `mise run check:wasm` builds the tests for
  wasm32-wasi and runs them under wasmtime.
- `mise run w3c` runs the W3C IRP suite (downloaded and converted on first
  use); every test not listed in `tests/w3c/skip.txt` must pass.
- `mise run lint` is the naming lint (ast-grep; always invoke `ast-grep`,
  never `sg`, which is shadow-utils' group tool in orbs).
- `mise run torture` opens the torture chamber with its dashboard;
  `mise run pg:test` tests the PostgreSQL binding against a throwaway
  server.
- `third_party/` is vendored (`mise run vendor-quickjs`): never edit or
  format it.

## Hard rules

- `src/base`, `src/image`, `src/core`, `src/system` are **freestanding**:
  only `<stddef.h>`/`<stdint.h>` and project headers. No libc, malloc, threads,
  clock, filesystem, recursion, or blocking.
- Layer direction: base ← image ← core ← system; base ← image ← compile;
  runtime depends on system/compile. Never the reverse.
- No pointers stored in mutable system memory: u32 indices, generation
  handles, or self-relative offsets (`ta_pointer_add`). Region must stay
  relocatable by `memcpy`.
- No allocation after `ta_system_init`. New capacity = new config field
  carved by the shared layout function (measure mode + real mode).
- Public ABI (`include/tinyactors/`): fixed-width ints, `uint32_t` instead of enums
  in structs, `uint32_t size` first in extensible structs, no callbacks except
  the datamodel vtable. Update `tests/unit/test_headers.c` ABI checks when
  touching it.
- Scheduler yields must never change SCXML-observable state (`_event`,
  queue order). Only semantic transitions touch `instance.current_event`.
- Every building block gets a unit test in `tests/unit/`.
- Names are whole words (docs/layout.md, "Naming"): the product is
  `tinyactors` (`<tinyactors/system.h>`, `libtinyactors_<layer>.a`), `ta_` /
  `TA_` is the only short prefix, files carry no prefix, internal includes are
  path-qualified (`"base/pool.h"`). Retired abbreviations (`len`, `buf`, `dm`,
  `fx`, `n_<things>`, …) fail `mise run lint`.
