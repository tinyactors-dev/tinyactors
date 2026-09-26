# ADR 0043: Definitions retire; definition IDs are generational

**Status:** Accepted; refines ADR 0028; refined by ADR 0077 (holds: a definition nothing holds is freed once no actor and no parent uses it; retiring drops every hold)
**Date:** 2026-09-19

## Context

A registered definition lives until `ta_system_destroy()`. Nothing removes
one. That leaks definition memory in situations that already exist:

- **Hot replacement** registers a new revision for each upgrade; the old
  revision stays registered after its last actor is gone.
- **Dynamic invocation**: an invoker that compiles child charts from
  `<content expr>` registers one definition per distinct child document,
  without bound.
- **Long-running hosts** load new charts over time.

An `unregister` call that fails with `TA_ERR_BUSY` while actors use the
definition forces hosts to poll, and it cannot express the replacement
pattern "stop new use now, drain the incumbents, free afterwards".

`ta_definition_id` is a 32-bit index into a table that only grows. Once a
definition can be freed, its slot can be reused, and a stale ID could name a
different definition.

## Decision

1. **`ta_system_retire_definition()`** ends a definition's availability:
   - new spawns and replacements that name it fail with `TA_ERR_NOT_FOUND`;
   - `ta_system_find_definition()` no longer returns it;
   - resident actors that use it keep running unchanged;
   - the core frees it when it is retired and no resident actor uses it
     (immediately, if none does).
   Retiring is idempotent. There is no un-retire; register the definition
   again instead.
2. **"Uses" means resident actors**, including unpublished replacements.
   Unloaded session images do not pin a definition: they carry its exact
   identity (ADR 0028, ADR 0038) and resolve it again on reconstruction.
3. **`ta_definition_id` becomes 64 bits: a slot and a generation**, the
   scheme `ta_actor_id` already uses. A freed slot's generation advances, so
   a stale ID fails with `TA_ERR_NOT_FOUND` instead of naming another
   definition.
4. `ta_definition_inspect()` (ADR 0042) reports `retired` and `actor_count`.

## Consequences

- Hot replacement: register the new revision, retire the old one, replace
  the actors; the old revision is freed with its last incumbent.
- Invokers and other caches of definition IDs must tolerate
  `TA_ERR_NOT_FOUND` and retire what they registered when torn down.
- Queries on a retired definition that still has actors keep working, so
  resident actors can still be inspected and exported.
- The ABI changes: every struct and function carrying `ta_definition_id`
  widens. Bindings must carry the ID as a 64-bit value (JSON-RPC carries
  values above 2^53-1 as decimal strings).
- System destruction still frees every definition, retired or not.
