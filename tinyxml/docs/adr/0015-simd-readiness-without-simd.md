# ADR 0015: SIMD readiness without SIMD

**Status:** Accepted  
**Date:** 2026-09-20

## Context

Scanning is where a fast XML parser spends its time, and vector instructions are what make it fast: find the next `<`, `&`, or `\r` in a 32-byte block with one comparison and a mask. simdjson and the Parabix work on bitstream XML parsing show the ceiling.

Chasing that ceiling now would be wrong. There is no implementation yet, the motivating consumer — SCXML documents of a few kilobytes, parsed once — would not notice, and vector code written before a correct scalar parser exists is code written against a moving target. But a design can foreclose SIMD by accident: a byte-at-a-time interface, unpadded buffers, character columns, or public pointers into internal storage each make vectorization a rewrite rather than a substitution. yxml, otherwise the closest library in spirit to txml, is unusable as a fast scanner for exactly the first reason.

## Decision

txml ships without SIMD. The decisions that keep it a drop-in are made now and are binding:

- **The staging window is padded** (ADR 0008), so a block scanner may over-read past the current position without leaving the library's memory and without a scalar tail special case.
- **Scanning is delimiter-driven per lexer state.** The scanner asks for the next occurrence of a small set of bytes — text wants `<`, `&`, `\r`; an attribute value wants its quote plus `<`, `&`, `\r`, and whitespace; a comment wants `-`; CDATA wants `]` — behind one internal entry point. That entry point is the seam: SWAR today, a vector kernel later.
- **Normalization happens on the copy into the pool** (ADR 0008), so scan, copy, and normalize are already one pass and one place.
- **Positions count bytes, not characters** (`txml_position`), so a line count is a popcount of a newline mask and a column is arithmetic. A character column would require decoding every byte and would defeat the mask.
- **Queries scan a name-ID column** (ADR 0004, ADR 0006): `txml_find_next` compares `uint32_t`s, which is the most obviously vectorizable loop in the library, and `TXML_NAME_NONE` lets it skip the scan entirely.
- **Callers hold indices, never pointers into columns** (ADR 0005), so splitting hot fields such as `name` into their own column — 16 candidates per cache line instead of about 2.7 — is private.

The portable baseline is SWAR over `uint64_t`, which needs no headers and runs on targets without SIMD, such as Xtensa-class microcontrollers. When kernels are added, they are selected at compile time for SSE2/AVX2, NEON, and WASM `simd128`; runtime CPU dispatch is not in the core, because feature detection is platform-specific and WASM has none. Intrinsic headers are compiler-provided, not libc, but the no-C-library rule in `AGENTS.md` still needs an explicit exception written down when that day comes.

The binding constraint: **adding SIMD must not change the public API.** If a proposed kernel requires a header change, the kernel is wrong.

## Consequences

The first implementation can be straightforward scalar code, which is what a correctness-first suite (ADR 0012) needs, without painting the project into a corner.

When throughput matters, the work is bounded and local: implement the delimiter scan and the name-column scan as vector kernels, behind the seams named above. No caller recompiles against a changed header, and no test changes, because the spec suite asserts documents rather than internal behaviour — which also makes it the differential test that a new kernel must pass unchanged.

The honest cost is that txml will not be competitive with a vectorized parser on text-heavy documents until that work is done. For the workloads that motivate the library, that is a trade worth making, and for the workloads that do not, the path is open rather than blocked.
