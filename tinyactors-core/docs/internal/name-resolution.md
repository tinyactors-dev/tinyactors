# Names and identity

Tinyactors uses several different notions of name; they must not be conflated.

## Definition family

Human/logical identity of a statechart family. Runtime `ta_definition_id` is a compact handle. Checkpoints carry a family and session schema; an implementation may accept more than one schema.

## Actor name

Optional durable symbolic identity scoped by definition family. Examples: `(subscription, "sub_123")` or `(customer, "cus_456")`. The core owns this logical protocol while a host-provided directory stores/resolves it. Anonymous actors have no symbolic name.

## Actor ID

Opaque numeric runtime identity used for efficient addressing. An implementation may pack a slot and generation so stale references cannot reach a newly allocated actor that reused the slot.

An actor ID exists only while the actor is resident. Unload invalidates it;
load allocates a new actor ID while preserving the logical actor address.

## Directory residency

A directory entry is `RESIDENT(actor_id)` or `UNLOADED`. `UNLOADED` reserves
the address and tells routing infrastructure to load the image the host kept
in this process rather
than deliver to a nonexistent runtime ID. Replacement and removal include the
expected old entry and must be atomic compare-and-swap operations in the host
directory implementation.

## State ID

Definition-local compact state identity. Human state names/IDs are definition metadata; runtime transition code should use compact IDs.

## Event name

Structured token path managed by `event_name.h`, distinct from ordinary atoms.

## Invocation/send/timer IDs

Protocol identities with subsystem-specific scoping and persistence requirements. A send ID is especially important because cancellation is scoped to the originating session.
