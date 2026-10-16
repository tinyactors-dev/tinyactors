# Protocol vectors

The bytes a host takes and gives across its API, written down case by case.
They are protocol tests, not conformance tests in the W3C sense: they fix
the encoding, not the SCXML semantics (those are the scenarios and the
W3C suite beside this directory). Every binding reproduces them through its own CBOR module and
its own calls into core: the same request bytes in, the same reply bytes
out ([ADR 0009](../../../docs/adr/0009-one-host-two-queues.md), decision 11).

| File | What | Read by |
|---|---|---|
| `requests.txt` | messages posted with `ta_host_post`, their replies, and the notifications of the runs between them (`tinyactors-core/docs/requests.md`) | the runner |
| `config.txt` | configs of `ta_host_create`, and the error it writes for a refused one (`tinyactors-core/docs/host-config.md`) | the runner |
| `values.txt` | neutral values (`tinyactors-core/docs/protocol.md`, Values), through the echo setup | the runner; core's `tests/unit/test_datamodel_ecmascript.c` too |

## Running them

Against an implementation, through the protocol it speaks
([docs/driver.md](../../docs/driver.md)):

    mise run //tinyactors-test:run -- --suite protocol <command…>

`mise run test` in `tinyactors-core` runs them against `ta-conform`, core's
reference implementation (`tinyactors-core/tools/conform/`). Alone, in
`tinyactors-core`:

    mise run build
    mise run //tinyactors-test:run -- build/dev/bin/ta-conform --suite protocol

A failure prints the expected and the actual bytes in diagnostic notation,
and the actual bytes in hex.

## The format

Each file starts with comments (`#`) that list its line kinds. A case is a
line `<kind> <name>` and the lines after it, up to a blank line. A line is
a key and a value: `send`, `reply`, `cbor`, `error` and `back`
hold CBOR in hex, and each is followed by `diag`, the same bytes in
diagnostic notation (`ta_cbor_diagnostic`), which the runner checks too, so
the hex and what a person reads cannot drift apart. A line `only
reference` limits a case to the reference (below).

## Reference-only cases

Some cases of `requests.txt` and `config.txt` send bytes that no binding's
API makes. A binding's typed method (one per method of requests.md that
its language has no direct call for, taking the method's params and
response handlers: ADR 0012, decision 3)
writes the request map itself, with the method's name and an id of its
own, and hands its handlers the result or the error; its host is created from a config
the binding encodes from a language value
(`tinyactors-core/docs/bindings.md`, sections 3 and 10). Most of these cases test
how core reads the envelope around a request, not a method; the others
test a method a binding offers as a direct call. Each has
the line `only reference`: the reference (`ta-conform`, `--kind
reference`) runs it, and a binding's run (`--kind binding`) skips it with
"reference only", as it skips `refused`. The runner does
not guess this from the bytes; the line says it. (The line is `only`, not
`kind`: a case's first word is already its kind.)

| Case | Why a binding cannot send it |
|---|---|
| `requests.txt` "not CBOR", `config.txt` "not CBOR" | bytes that are no CBOR item; a binding encodes a value |
| "not a map", "a batch is not supported yet" | a request is always one map |
| "no method, no id", "no method" | the binding always writes `method` (a missing one would be `null`, answered `type`) |
| "a member a request does not have" | the binding writes only `id`, `method` and `params` |
| "id is not an integer, text or null", "id is not a float" | the id is the binding's own |
| "id: an unsigned integer", "id: a negative integer", "id: text", "id: null" | the id is the binding's own: the adapter writes it back, so the case would test the adapter, not core |
| "more items than a request holds" | no id, answered `"id": null`; the binding sends an id |
| "notification: no reply", "notification: no error either", "notification: applied", "notification: a request that is not one is answered" | messages without an id (JSON-RPC's notifications, not the host's NOTIFICATION effects): the binding's request always has an id and wants the reply |
| "actor.send: data that is no neutral value" | a binding's typed method takes the language's values, which are all neutral values: it cannot write tag 99 |
| the other `actor.send` cases | a binding mirrors a method with a typed method only where its language has no direct call: a program sends with the binding's `send` (`ta_host_resolve` and `ta_host_send`); `actor.send` is for a client outside the process |
| "method is not text", "no such method", "params is not a map" | a binding has no raw post (ADR 0012, decision 3): its typed methods, one per method of requests.md, write the method's name and the params map themselves |

A binding passes the caller's values through unchecked, so the cases of
the method level stay a binding's: a param a method does not take
(`params.x`, a keyword its typed method passes through), a param of the wrong type or out of range, the
limits on params (`actor.list`'s 512, "more than 32 rules"), every
method's results, `host.config`, and every config but "not CBOR" (a config
that is not a map is the language's `1`).

## Writing a case

Write the `diag` line, then let the task write the hex above it:

    mise run //tinyactors-test:encode           # rewrite every hex line from its diag line
    mise run //tinyactors-test:encode --check   # change nothing; fail where they differ

It encodes as core does: shortest integers and lengths, definite lengths,
map keys in the order written, floats in the shortest exact width and NaN as
`f97e00`. It reads what `ta_cbor_diagnostic` writes, plus tags (`24(...)`),
byte strings (`h'..'`), `simple(n)`, `undefined`, `NaN`, `Infinity` and
`-Infinity`. A `diag` line in parentheses, such as `diag (not CBOR)`, is not
an item: its hex stays as written. The check also reads each hex line back
as `ta_cbor_diagnostic` writes it and compares that with its `diag` line.
`mise run test` in `tinyactors-test` runs the check.

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
them have `ticksPerSecond` 1000, `maxActors` 8, `traceSeed` 1
and no telemetry encoder. The charts are in
`setups/`; after spawning, a setup runs the host until it is idle with the
wall clock at 1700000000000000000 ns. The runner builds them with the
driver's methods (`lib/runner.rb`, `setup` and `echo`).

The setups of `requests.txt` run the ECMAScript
datamodel (quickjs, whatever datamodel the runner was given: their charts,
`setups/*.scxml`, are ECMAScript). `values.txt` runs on the echo setup,
on the datamodel the runner was given: its chart, `setups/echo.txml`, is
written in the `conf:` notation and converted for that datamodel's
language like the scenarios (`build/setups/<language>/`, checked by
`mise run check-languages`).

| Setup | The host |
|---|---|
| `empty` | system `h`, no chart (the default) |
| `pinged` | system `h`, the chart `pinged` (`<data>` `n` = 1, `who` = `"bob"`, `nothing`; states `s`, `t`) and one session, 1, run until idle |
| `pinged2` | as `pinged`, with two sessions, 1 and 2 |
| `echo` | system `h` on the datamodel under test, with no I/O processor; the chart `echo` (each event `v` is sent back to its `_event.origin` as `back`, with `_event.data` as its content) and one session; a case sends it `v` with the value as `cbor` and the origin `tinyactors://p/#_scxml_9` (`actor.send`), runs the host once (`host.run`) and compares the data of the one send to the host's transport |

## What they leave to unit tests

What bytes cannot show stays in `tinyactors-core/tests/unit`: a call during a run
(`test_host_inputs.c`), an allocator that refuses (`test_host_memory.c`),
`actor.context` of a hibernated actor or of a datamodel without variables,
a post's REPLY after its method's own effects (`test_host_request.c`),
and sends to other systems through the host's transport
(`test_remote.c`).
