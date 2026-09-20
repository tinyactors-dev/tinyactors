# ADR 0030: Interpreter extensions cannot bypass statechart configuration semantics

**Status:** Accepted; reconstruction wording refined by ADR 0038; subject
narrowed by ADR 0046, which removes the extension action — the rule now
describes I/O processors and invokers, which cannot change a configuration
by construction  
**Date:** 2026-09-15

## Context

Custom executable content and host callbacks are useful, but allowing extensions to directly mutate the active configuration would make ordering, history, parallel exit/entry behavior, inspection, and verification incoherent.

## Decision

Extensions may evaluate data, perform host work, raise/send events, and use supported executable-content hooks, but state configuration changes occur only through the interpreter's transition semantics. There is no public `goto_state()` escape hatch.

## Consequences

Control flow remains inspectable and follows one semantic engine even when data and I/O are highly extensible. Session-image reconstruction restores configuration through that same interpreter-owned representation. Host object graphs remain bound through the data-model adapter rather than being serialized by extensions.
