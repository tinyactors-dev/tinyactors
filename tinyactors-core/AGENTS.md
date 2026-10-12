# AGENTS.md

Portable, embeddable SCXML interpreter + cooperative actor-system VM in C.
Read `docs/architecture.md` before changing runtime code; `docs/layout.md`
lists where things go.

## Commands (all via mise)

The repository root's AGENTS.md applies too: tasks are Ruby file tasks in
`mise/tasks/`, and the build goes through the root's pinned toolchain
(`mise/lib/toolchain/`). `mise/core.rb` lists the layers, the test
programs and the builds (`dev`: ASan/UBSan at -O1; `release`; `wasi`).

- `mise install` (at the root) installs the pinned toolchain.
- `mise run build` builds every layer, test program and tool
  (`build/dev/lib`, `build/dev/bin`); `--release` builds `build/release`.
- `mise run test` builds and runs every test program, then `lint`,
  `check:freestanding`, `api --check` (after `amalgamate`), `wasm:test`,
  `conform` and `check:wasm`; `--full` adds `torture:test` and `check:cross`. Each also runs on its own.
- `mise run check:freestanding` must pass for any change under
  `src/{base,image,core,system}`; `mise run check:wasm` builds the tests for
  wasm32-wasi and runs them under wasmtime.
- `mise run conform` runs the conformance suite (`../tinyactors-test`:
  protocol vectors, scenarios, the W3C IRP suite) against `ta-conform`
  (`tools/conform/`), core's reference implementation of the suite's
  driver protocol: on quickjs in both realms, and W3C on the tests'
  datamodel, but for the tests `tests/support/datamodel_test_w3c_skip.txt`
  names. Every case must pass. A change to `docs/driver.md` changes
  `ta-conform` in the same commit.
- `mise run lint` runs every ast-grep rule in `tools/lint/rules/` over the
  C sources, after their tests in `tools/lint/tests/` (always invoke
  `ast-grep`, never `sg`, which is shadow-utils' group tool in orbs). A new
  rule file is picked up without editing a list:
  - `no-retired-abbreviations`: names are whole words;
  - `at-most-three-parameters`: a public function takes at most three;
  - `buffers-are-ta-buffer`: bytes cross the public API as `ta_bytes` or
    `ta_buffer *`, not a pointer and a length (ADR 0009, decision 8).
- `mise run torture` opens the torture chamber with its dashboard.
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
- A rarely called host operation is a method of `ta_host_request`: follow
  "Adding a method" in `docs/requests.md` for what belongs there and how.
- Bytes that cross the API (requests, configs, envelopes, values) are
  fixed by the protocol vectors in `../tinyactors-test/suites/protocol/`
  (its README.md): a
  change to them changes a vector, and every binding follows it.
- A change a binding must follow (a call, an effect, a method, the config)
  updates the binding guide, `docs/bindings.md`, in the same commit.
- Names are whole words (docs/layout.md, "Naming"): the product is
  `tinyactors` (`<tinyactors/system.h>`, `libtinyactors_<layer>.a`), `ta_` /
  `TA_` is the only short prefix, files carry no prefix, internal includes are
  path-qualified (`"base/pool.h"`). Retired abbreviations (`len`, `buf`, `dm`,
  `fx`, `n_<things>`, …) fail `mise run lint`.
