# Protocol vectors

The bytes a host takes and gives across its API, written down case by case.
They are protocol tests, not conformance tests in the W3C sense: they fix
the encoding, not the SCXML semantics (those are `tests/conformance` and
`tests/w3c`). Every binding reproduces them through its own CBOR module and
its own calls into core: the same request bytes in, the same reply bytes
out ([ADR 0009](../../../docs/adr/0009-one-host-two-queues.md), decision 11).

| File | What | Read by |
|---|---|---|
| `requests.txt` | requests and replies of `ta_host_request` (`docs/requests.md`) | `run_protocol.c` |
| `config.txt` | configs of `ta_host_create`, and the error it writes for a refused one (`docs/host-config.md`) | `run_protocol.c` |
| `envelopes.txt` | envelopes and error envelopes between systems (`docs/protocol.md`, Envelope) | `run_protocol.c` |
| `values.txt` | neutral values (`docs/protocol.md`, Values) | `tests/unit/test_datamodel_ecmascript.c` |

## Running them

`mise run test` in `tinyactors-core` runs them (the program `protocol`). Alone:

    mise run build
    build/dev/bin/protocol tests/protocol

A failure prints the expected and the actual bytes in diagnostic notation,
and the actual bytes in hex.

## The format

Each file starts with comments (`#`) that list its line kinds. A case is a
line `<kind> <name>` and the lines after it, up to a blank line. A line is
a key and a value: `send`, `reply`, `cbor`, `error`, `envelope` and `back`
hold CBOR in hex, and each is followed by `diag`, the same bytes in
diagnostic notation (`ta_cbor_diagnostic`), which the runner checks too, so
the hex and what a person reads cannot drift apart.

## Wildcards

Where a value depends on the build, the layout or a generator, the expected
side holds a wildcard: tag 39999 on a text naming the type, which matches
any one item of that type at that place.

| Diagnostic notation | Matches |
|---|---|
| `39999("uint")` | an unsigned integer |
| `39999("int")` | any integer |
| `39999("text")` | text |
| `39999("bytes")` | a byte string |
| `39999("float")` | a float |
| `39999("any")` | any one item |

Tag 39999 is unassigned in IANA's CBOR tags registry, and core never writes
it. Wildcards stand for: an error's `message` (for people, not to match on),
`regionBytes`, `fuel` and `fuelClock` (they move with the layout and the
interpreter), the `actors` cursor (opaque), and a generated `traceparent`.
Everything else is pinned: system ids, the trace seed, the clock.

## Setups

A case names the host it runs on (`setup`); each case gets a new one. All of
them run the ECMAScript datamodel, with `ticksPerSecond` 1000, `maxActors` 8,
`traceSeed` 1, `maxSubscriptions` 1 and no telemetry encoder.

| Setup | The host |
|---|---|
| `empty` | system `h`, no chart (the default) |
| `pinged` | system `h`, the chart `pinged` (`<data>` `n` = 1, `who` = `"bob"`, `nothing`; states `s`, `t`) and one session, 1, run until idle |
| `pinged2` | as `pinged`, with two sessions, 1 and 2 |
| `pinger` | system `a`, a peer `b`, the chart `pinger` and one session that was sent `go` with `{to: "tinyactors://b/@echo", n: 1}`: its run sends `ping` (with data) and `note` (without) to `b` |
| `relay` | system `b` with the peers `a` and `c`, no chart |

## What they leave to unit tests

What bytes cannot show stays in `tests/unit`: a call during a run
(`test_host_inputs.c`), an allocator that refuses (`test_host_memory.c`),
`actor.context` of a hibernated actor or of a datamodel without variables,
CHANGED effects (`test_host_request.c`), and moving envelopes between hosts
(`test_router.c`).
