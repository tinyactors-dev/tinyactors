# Names and identity

Tinyactors uses several different notions of name; they must not be conflated.

## Definition name

Human/logical identity of an immutable statechart definition. Runtime `ta_definition_id` is a compact handle. Persistent snapshots need a stable definition identity/revision resolver.

## Actor name

Optional durable symbolic identity scoped by definition. Examples: `(subscription, "sub_123")` or `(customer, "cus_456")`. Anonymous actors have no symbolic name.

## Actor ID

Opaque numeric runtime identity used for efficient addressing. An implementation may pack a slot and generation so stale references cannot reach a newly allocated actor that reused the slot.

## State ID

Definition-local compact state identity. Human state names/IDs are definition metadata; runtime transition code should use compact IDs.

## Event name

Structured token path managed by `event_name.h`, distinct from ordinary atoms.

## Invocation/send/timer IDs

Protocol identities with subsystem-specific scoping and persistence requirements. A send ID is especially important because cancellation is scoped to the originating session.
