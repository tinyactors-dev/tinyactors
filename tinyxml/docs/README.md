# txml design documentation

This documentation describes the intended architecture of **txml**, a freestanding XML reader and writer that performs no I/O, uses no C library, and never relocates what it allocates.

The documentation is split by audience:

- `adr/` — architecture decision records. These are the primary record of design intent.
- `internal/` — implementation reference, memory architecture, invariants, and module responsibilities.

txml exists because `tinyactors-core` deliberately has no XML dependency (core ADR 0001) and defers an SCXML importer/exporter to a separate component (core ADR 0025). txml is that XML front end, built to the same constraints as the core: allocator injection, no libc, no I/O, no hidden global state.

The W3C XML 1.0 Recommendation and Namespaces in XML 1.0 are the syntactic references: <https://www.w3.org/TR/xml/> and <https://www.w3.org/TR/xml-names/>. ADR 0012 states exactly how much of them txml implements.

## Status

`include/txml.h` is authored and complete; `src/` is empty. There is no implementation yet.

The ADRs below are the design half of the project. The spec suite in `tests/spec/` is the executable half: each case states an input document and the node table, names, and errors a conforming implementation must produce. The suite is written against the header and is expected to fail until the implementation exists — a pending suite is the correct state, and no test may be weakened to make it pass.

`AGENTS.md` in the project root carries the hard constraints that every change must respect.
