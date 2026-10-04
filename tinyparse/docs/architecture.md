# tinyparse architecture

This document describes how `tinyparse` is built and how its pieces fit. The
*what* and *why* of the public contract live in
[`../tinyparse-design.md`](../tinyparse-design.md); this is the *how*. The
build schedule is [`build-plan.md`](build-plan.md).

## Layers

```
        ┌─────────────────────────────────────────────┐
        │  tinyparse CLI  (hosted: libc, argv, IO)   │  src/cli.c
        ├─────────────────────────────────────────────┤
        │  libtinyparse  (freestanding: no libc)     │  src/*.c, include/tinyparse.h
        │   reader → IR → lowering → scanner → CST      │
        ├─────────────────────────────────────────────┤
        │  ax actor core   (freestanding)              │  dist/<plat>/libax.a + ax.h
        └─────────────────────────────────────────────┘
```

The library is **freestanding C** — no libc, no `malloc`, no IO — exactly like
`ax`. It receives a host memory grant and is pumped by the host. The CLI is the
hosted shell around it: it reads files, renders diagnostics, and prints. The
two never blur: nothing under `src/` except `cli.c` may call libc.

## Consuming the `ax` release artifacts

`tinyparse` does not recompile `ax`. It links the prebuilt release archive
`dist/<platform>/libax.a` and includes `dist/<platform>/ax.h`. `build.sh`
populates `dist/` (via the repo's `ci/zig-build.sh` / `make libs`) if it is
empty, then links against it. The marker `dist/<platform>/.consumes-ax`, written
only after the CLI links successfully against that archive, is the integration
proof the test suite checks. Grammar rules lower to `ax` machines: a rule is an
actor spec, nested rules are child actors, token events drive transitions, and
the engine is pumped to quiescence per push.

## Toolchain

The library and CLI are built with **Zig 0.15.1** (`zig cc` / `zig ar`), pinned
in [`../mise.toml`](../mise.toml) and provisioned by
`scripts/tinyparse-setup.sh`. Zig cross-compiles the deliverable matrix from
one host: `x86_64-linux-gnu` and `aarch64-macos-none`. This is deliberately
independent of the repo's 0.16 freestanding toolchain; static-archive linking
against `libax.a` is ABI-stable across the two Zig versions.

## The pipeline (per the design doc)

1. **Reader** — parse `.scm` bytes into Scheme-shaped forms.
2. **Validate + intern** — check the grammar, intern names to symbols.
3. **IR** — compile tokens, lexical machines, and rules to an intermediate
   representation.
4. **Lowering** — lower the IR to `ax`-shaped machines and define them.
5. **Scanner** — choose a lexical goal, produce token/event records.
6. **Parse** — post token events to the actor graph, pump `ax`, append CST
   records to contiguous arrays.
7. **Output** — a contiguous-array concrete syntax tree with cursor-only
   navigation, plus structured diagnostics.

## The finish line

Development is gated by a **data-driven, ratcheting test suite** (`test/`,
interpreted by `test/harness.c` against `test/xfail.list`) and the metric
`ci/tinyparse-metric.sh`. Tests are data, never code; the metric goes green
only when every test passes and the ledger is empty. This gate outlives the
build and is the standing guard against regressions.

## Source layout

```
tools/tinyparse/
  tinyparse-design.md   canonical design (the contract)
  include/tinyparse.h   public API (regular C)
  src/                    libtinyparse + cli.c (regular C, not literate)
  docs/                   this doc, the build plan, grammar + CLI guides
  examples/               sexpr, tcl, rewriter, ebnf, uri, jit
  test/                   harness.c, suite/**/*.test (data), xfail.list
  build.sh                portable build engine
  mise.toml               Zig 0.15.1 + tasks
```
