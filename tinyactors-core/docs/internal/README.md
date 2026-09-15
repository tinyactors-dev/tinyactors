# Internal reference

This directory describes implementation responsibilities and invariants. It follows the current header-module split and the accepted ADRs.

Important cross-cutting references:

- `semantic-state.md` — what constitutes a running actor/session.
- `interpreter-semantics.md` — macrostep/microstep execution.
- `invariants.md` — rules implementations must never violate.
- `memory-model.md` — density strategy for millions of actors.
- `persistence-format.md` — snapshot and identity requirements.
- `api-gaps.md` — current header skeleton vs intended design.
- `verification.md` — simulator/fuzzer integration.
- `scxml.md` - the SCXML standard from https://w3.org/TR/scxml, as markdown.
