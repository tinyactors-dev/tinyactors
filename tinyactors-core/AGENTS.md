# C coding rules

- never use malloc/free, stdlib or other implementation-providing C headers
- stdint, stdefs, and stdbool are okay
- functions are allowed a maximum of three parameters; introduce struct types if you need more.
- the public API is `include/tinyactors.h` only: declare every public function there with `TA_API`; `src/*.h` headers are private.
- run `mise run amalgamate` after changing any header or source; never edit `include/tinyactors.c` by hand.
- run `mise run api` after changing `include/tinyactors.h`; annotate new declarations with the `TA_*` macros it asks for (see `docs/internal/api-ir.md`), and run `mise run api-test` after changing the extractor in `mise/lib/api`.
