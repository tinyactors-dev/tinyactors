# The host kernel: C rules

The kernel follows core's rules (kernel ADR 0001, section 2), because it
must build wherever core does, the browser included.

- **No C library.** No malloc/free, no `string.h`, no stdio: only the
  freestanding headers (`stdint.h`, `stddef.h`, `stdbool.h`). The build
  compiles `src/` with `-ffreestanding -nostdlibinc`, and
  `mise run check-freestanding` compiles it for `wasm32-freestanding` and
  fails on any undefined symbol other than core's API (`ta_*`), the
  kernel's own (`tah_*`) and the compiler's memory builtins. Use
  `src/memory.h` for copies and comparisons.
- **Platform functionality is an interface.** A clock, a timer, an event
  loop, a language's values, anything that talks to the outside: declare
  it in `include/tinyactors_host.h` as an adapter (`self` plus a vtable)
  that the binding implements. Never call the platform from `src/`.
- **Memory only through the allocator the binding gives the kernel**
  (core's `ta_allocator`).
- **No recursion.** `mise run check-recursion` builds the kernel's call
  graph and fails on any cycle (core ADR 0060 applies here too).
- **At most three parameters per function;** introduce a struct for more.
- **One public header:** `include/tinyactors_host.h`, prefix `tah_`.
  `src/*.h` headers are private.
- **Nothing of any language** (Node-API, Python, Ruby) enters the kernel:
  that belongs in a binding (README, "Layers and responsibilities").
- Tests go in `tests/*_test.c`; they may use the host's C library, and
  `mise run asan` runs them under AddressSanitizer and UBSan.

Decisions are recorded in `docs/adr/`, append-only as core's are: an
accepted ADR's Decision is not rewritten; implementation notes and Status
pointers are the only later edits.
