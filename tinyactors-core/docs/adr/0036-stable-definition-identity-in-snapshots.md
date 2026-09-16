# ADR 0036: Stable definition identity in snapshots

- Status: Superseded by ADR 0037; exact-revision identity reinstated by ADR 0038
- Date: 2026-09-15

## Context

Snapshots must survive process restart, independently rebuilt atom tables, and different registration orders. `ta_definition_id` and `ta_atom` are compact runtime handles, so persisting the values directly can restore against the wrong definition or name.

The state configuration uses definition-local IDs. Those IDs are safe to reconstruct only when the recovered definition is exactly the definition that produced the image.

## Historical decision

Every immutable definition has a persistent definition key consisting of a logical definition family and stable revision fingerprint. Recovery resolves that exact key before decoding definition-local state.

ADR 0037 temporarily replaced exact revisions with a family/session-schema compatibility model. ADR 0038 returns to the exact-revision rule because upgrades are actor replacement, while reconstruction means continuing the same interpreter session.
