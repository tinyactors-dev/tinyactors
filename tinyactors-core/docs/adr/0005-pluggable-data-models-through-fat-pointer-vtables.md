# ADR 0005: Pluggable data models through fat-pointer vtables

**Status:** Accepted; persistence portion superseded by ADR 0038  
**Date:** 2026-09-15

## Context

SCXML intentionally abstracts expression and data semantics. Embedders need native integration with Python, Ruby, JavaScript, compact embedded data, and custom application models.

## Decision

A data model is represented as `{self, vtable}` and supplies definition preparation, per-instance storage sizing/initialization, expression evaluation, location access/assignment, script execution, and an optional inspection root. The core does not define an expression language.

ADR 0038 supersedes the original requirement that every data model implement snapshot/restore. Host object graphs remain host-owned and are rebound when reconstructing a Tinyactors session.

## Consequences

Embedders are not artificially constrained. Arbitrary data-model behavior can make replay or analysis less complete; that is a property of the chosen model, not a core restriction.
