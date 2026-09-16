# ADR 0036: Stable definition identity in snapshots

- Status: Superseded by ADR 0037
- Date: 2026-09-15

## Context

Snapshots must survive process restart, independently rebuilt atom tables, and
different registration orders. `ta_definition_id` and `ta_atom` are compact
runtime handles, so persisting the values in `ta_snapshot_header` would permit
an actor to restore against the wrong definition or name.

The state configuration uses definition-local IDs. Those IDs are safe to
restore only when the recovered definition is exactly the definition that
produced the snapshot. Tinyactors does not yet define automatic statechart
migration.

## Decision

Every immutable definition has a persistent definition key:

- a logical definition name, encoded as UTF-8 bytes; and
- a stable revision fingerprint, encoded as opaque bytes.

The embedder/build pipeline supplies the revision fingerprint, or derives it
from a canonical compiled-definition artifact. It must change when a change
can alter snapshot interpretation. It must not be derived from runtime
handles, memory addresses, registration order, or callback addresses.

Definition registration retains and indexes this key. Recovery resolves the
key to a registered definition before decoding definition-local session state.
A missing key fails recovery. A matching logical name with a different
revision is a definition mismatch and also fails recovery; it is not silently
treated as an upgrade.

The snapshot wire format encodes the definition key and optional actor name as
length-prefixed logical bytes. It does not serialize `ta_definition_id` or
`ta_atom`, and it does not serialize a native C header struct directly.
Runtime atoms are interned only after their logical strings have been decoded.
Structured event names are likewise encoded as logical segments rather than
`ta_event_name_id` values.

Named actors have the durable symbolic address `(definition logical name,
actor name)`. `ta_actor_id` may be included as an optimization only when the
selected store guarantees that its allocation scheme is stable across restart.
An anonymous actor that must be persisted needs a store-owned durable key;
otherwise it is not recoverable by identity after restart.

## Consequences

`ta_snapshot_header` becomes an in-memory representation or is replaced by a
wire-format-specific decoded header; its current runtime-handle fields are not
the persisted representation.

The public definition/build API needs a way to provide and inspect the stable
definition key. The recovery path needs a distinct definition-mismatch result
so a host can distinguish an unavailable definition from an incompatible
revision.

Persisted configuration, history, and other definition-local IDs remain dense
and need not be converted to state names, because the exact revision check
protects their interpretation. A future definition-migration feature must be
an explicit host-directed operation that reads an old revision and writes a
snapshot for a new revision.

## Related decisions

This completes the open stable-definition-revision detail in ADR 0028 and
refines the identity requirements of ADRs 0004 and 0013.
