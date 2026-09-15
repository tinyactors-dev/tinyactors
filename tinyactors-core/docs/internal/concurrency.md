# Concurrency model

TA provides actor isolation semantically but does not own host concurrency.

## Per actor

Only one macrostep may be active. A paused actor is still running that macrostep. No second external event may enter it.

## Per system

Assume mutation is single-threaded/external-synchronized. If multiple threads call one system concurrently, the embedder must prevent races unless a future explicitly thread-safe API is introduced.

## Scaling

A server may:

- use one event-loop thread for a system;
- shard actor ownership across N systems/threads;
- route `(definition, name)` to a shard;
- transfer ownership only at stable boundaries if migration is implemented.

Cross-actor event ordering is intentionally determined by the host scheduler. This nondeterminism is useful input to simulation/fuzzing.
