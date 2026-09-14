# TinyActors::Raw::ActorRef

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result.
C anchors: `ta_actor_ref_t`, `ta_post_options_t`, `ta_upgrade_options_t`.

## Owned files

- `ext/tinyactors_raw/raw_actor_ref.c`
- `test/raw/actor_ref_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`ActorRef.new(system, id)` creates an immutable system-scoped identity.
Expose `system`, `id`, equality, and hash.
Accept the full uint64 range. Zero remains the C sentinel when a record permits it.
No ActorRef owns an actor lifecycle merely because it holds an ID.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_actor_spawn`
- `TinyActors::Raw.ta_actor_inspect`
- `TinyActors::Raw.ta_actor_set_limits`
- `TinyActors::Raw.ta_actor_stop`
- `TinyActors::Raw.ta_actor_release`
- `TinyActors::Raw.ta_name_bind`
- `TinyActors::Raw.ta_name_unbind`
- `TinyActors::Raw.ta_name_lookup`
- `TinyActors::Raw.ta_session_lookup`
- `TinyActors::Raw.ta_post`
- `TinyActors::Raw.ta_actor_upgrade`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Check system identity before every operation that receives a reference.
2. Keep exact-instance routing distinct from name lookup and session lookup.
3. Preserve spawn input snapshots and asynchronous initialization semantics.
4. Return complete actor information with copied text and byte views.
5. Preserve the borrowed nature of native state. Do not silently snapshot it in Raw.
6. Return status failures for stale or terminated actors.
7. Implement migration through protected callbacks and scoped memory.
8. Do not fabricate origin metadata for ta_post.

## Acceptance checks

- Deliver to two systems with equal numeric IDs without cross-routing.
- Rebind a name and verify the old reference still identifies the old incarnation.
- Stop and release an actor, then verify stale-reference behavior.
- Post native and byte payloads with snapshot checks.
- Upgrade successfully and verify failed migration rollback.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
