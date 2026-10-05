# Internal reference

This directory describes implementation responsibilities and invariants. It follows the current header-module split and the accepted ADRs.

Important cross-cutting references:

- `semantic-state.md` — Tinyactors-owned interpreter state vs host-owned data-model state.
- `session-image.md` — capture/reconstruction boundary.
- `datamodel-binding.md` — host object binding and large-data-model behavior.
- `durability.md` — optional portable checkpoint/replay/effect protocols above core.
- `interpreter-semantics.md` — macrostep/microstep execution.
- `routing.md` — normalized external-event scheduling boundary.
- `names.md` — which names are interned and when, and how an event's name is matched by its atoms.
- `telemetry.md` — trace records on the lossy `tel` ring: what they carry, the parameters, and how hosts read them.
- `invariants.md` — rules implementations must never violate.
- `memory-model.md` — density strategy for millions of actors.
- `verification.md` — simulator/fuzzer integration.
- `testing-and-api-change.md` — how to grow the test suite so it catches bugs,
  the traps in doing that, and when and how to change the public API.
- `api-ir.md` — `include/tinyactors.api.json`, the machine-readable public API for binding generators.
- `scxml.md` — the SCXML standard from https://w3.org/TR/scxml, as markdown.
