# ADR 0005: Pluggable data models through fat-pointer vtables

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML intentionally abstracts expression and data semantics. Embedders need native integration with Python, Ruby, JavaScript, compact embedded data, and custom application models.

## Decision

A data model is represented as `{self, vtable}` and supplies definition preparation, per-instance storage sizing/initialization, expression evaluation, location access/assignment, script execution, and snapshot/restore. The core does not define an expression language.

## Consequences

Embedders are not artificially constrained. Arbitrary data-model behavior can make replay or analysis less complete; that is a property of the chosen model, not a core restriction.
