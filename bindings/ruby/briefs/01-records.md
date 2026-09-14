# Infrastructure: raw record conversion

Owner: record adapter task. Status: planned.
Dependencies: [bootstrap](00-bootstrap.md), Raw::Result, Raw::Value, and Raw::Memory.
Read [CONTRACT.md](CONTRACT.md) first.

## Outcome and files

Implement all public descriptor and view conversions in `ext/tinyactors_raw/records.c` and `records.h`.
Own `test/raw/records_test.rb`.
Also own the struct inventory at `briefs/raw-records.md`.
The bootstrap integrator approves shared declarations and registers the initializer.
Record adapters create no public descriptor classes.
Use the bootstrap handle factories. Do not depend on completed resource procedures.
This assignment owns `TinyActors::Raw.ta_target_parse(raw_text)`.
Return its exact status and target record through Raw::Result.

## Work

1. Inventory every public struct and union in `src/tinyactors.h`.
2. Record each struct's input/output use and ownership in `briefs/raw-records.md`.
3. Convert Hashes using exact C names and active union members.
4. Convert nested charts and normalized definitions without losing authored order.
5. Copy callback views before their C lifetimes end.
6. Root native values and descriptor code owners through the documented lifetime.
7. Preserve ordered parameter duplicates, trace metadata, and invocation identity.
8. Validate counts, enum ranges, recursion depth, and integer bounds before calling C.

The inventory must cover arenas, limits, payloads, actions, charts, definitions, invokes, callbacks, execution, inspection, I/O, and migration.
Derive counts from Ruby Arrays. Do not accept interior sentinel entries.
Preserve absent versus empty text and absent versus native-nil payloads.
Marshal every `ta_event_t` field, even when application data conversion fails.
Return immutable record containers without freezing live actor state.
Keep mutable byte views behind scoped Raw::Memory objects.

## Acceptance

- Test all tagged-union branches and optional fields.
- Round-trip every public descriptor family through a real raw procedure where possible.
- Reject unknown fields instead of dropping them.
- Check byte payloads containing NUL and non-UTF-8 bytes.
- Exercise COPY and BORROW definitions with GC and source mutation.
- Verify origin, origintype, sendid, invokeid, message_id, and trace independently.
- Verify diagnostics remain readable after later native calls.

Do not implement a statechart compiler in this adapter.
Do not implement convenience aliases or status-to-exception policy.
