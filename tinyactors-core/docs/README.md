# Tinyactors Core design documentation

This documentation describes the intended architecture of **Tinyactors**, a tiny in-process actor/statechart library whose execution semantics are based on SCXML 1.0 while deliberately not depending on XML.

The documentation is split by audience:

- `manual/` — minimal embedder-facing guidance.
- `adr/` — architecture decision records. These are the primary record of design intent.
- `internal/` — implementation reference, invariants, module responsibilities, and notes about the current header skeleton.

The W3C SCXML 1.0 Recommendation is the semantic reference for statechart behavior: <https://www.w3.org/TR/scxml/> (also available locally in docs/internal/scxml.md). 
Tinyactors adopts the execution model, not the XML surface. 

An SCXML importer/exporter can be added as a separate component later.

## Status

The C headers are currently a design skeleton. The ADRs distinguish accepted
design decisions from implementation work that remains. `internal/api-gaps.md`
records how previously identified public-surface gaps were closed.
