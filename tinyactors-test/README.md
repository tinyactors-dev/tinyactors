# tinyactors-test

`tinyactors-test` is the instrumentable host used to verify Tinyactors. It
links the production `tinyactors-core` amalgamation and exposes a persistent
JSON-RPC 2.0 subset over newline-delimited JSON on standard input and output.
Human-readable diagnostics are written to standard error.

The Ruby driver owns scenario descriptions, scheduling choices, assertions,
generation, shrinking, and reports. The C host owns Tinyactors objects, test
adapters, measurements, and calls into the production API.

`rpc.h` and `rpc.c` implement reusable JSON-RPC parsing, dispatch, responses,
errors, and notifications. `server.h` and `server.c` contain the
`tinyactors-test` lifecycle and the hand-written host methods. The method table
merges those `{name, handler, schema}` entries with the generated ones; a libjq
`jv` object maps method names to table entries, and discovery is derived from
the same table.

## Generated bindings

The host does not bind the Tinyactors C API by hand. The pipeline is:

```text
tinyactors-core/include/tinyactors.h
  -> mise run api        (core: clang JSON AST + api/annotations.rb)
  -> tinyactors-core/include/tinyactors.api.json   (checked in)
  -> mise run bindings   (this project: mise/tasks/bindings + bindings.rb)
  -> build/gen/ta_rpc.{c,h}                        (generated, not checked in)
```

`mise run build` runs both steps. The generator emits a JSON decoder and
encoder for every public type, a `codec.roundtrip.<type>` method per struct,
and one `ta.<domain>.<rest>` method per public function. A function that core
does not implement yet is still bound: its method validates params and then
answers error -32001, and discovery lists it under `unimplemented`.

`bindings.rb` is the binding policy. Every public function and type must be
generated, overridden by hand, or skipped with a reason; otherwise the
generator fails, so new core API cannot silently go unbound. Hand-written
parts live in `src/binding.{h,c}` (request arena, handle table, primitive
codecs, and the `ta_str`, `ta_bytes`, `ta_value`, and `ta_expression`
overrides) and `src/adapters.c` (the `$adapter` registry). The generator also
writes `build/gen/coverage.json` with the bound, skipped, and unimplemented
counts.

See `docs/protocol.md` for method naming, result shapes, value encoding,
handles, and adapters.

The host includes a jq-backed JSON data model. Source expressions and scripts
are jq filters over the actor's JSON root. A condition uses jq truthiness, an
assignment evaluates its expression as a jq location, and a script must produce
exactly one replacement root. Values use Tinyactors' normal retain, release,
and clone contract.

Named SCXML parameters become a JSON object keyed by parameter name, so `<param
name="x">` reads as `$_event.data.x`; a later duplicate name replaces an earlier one.

Every jq program sees the SCXML system variables `$_event`, `$_sessionid`,
`$_name`, and `$_ioprocessors`, read through `ta_dm_host_text`, and can call
`In("id")`. `$_event` is null before the first event. Literal `<content>`
is parsed as JSON, or else becomes a whitespace-normalized string.

## Commands

From the repository root:

```text
mise run build
mise run test
```

The hand-written host methods are:

- `rpc.discover`
- `system.ping`
- `json.transform`
- `metrics.snapshot`
- `host.shutdown`
- `system.actors` (resident actor IDs; `"runnable": true` lists only those with a macrostep in progress)
- `handle.fixture`

Every other method is generated (see above). From Ruby,
`driver.ta.actor.spawn(...)` and `driver.codec.roundtrip.ta_scxml_spec(value: ...)`
call methods through a proxy built from `rpc.discover`, which rejects unknown or
missing arguments before sending. `ruby/bindings_test.rb` covers discovery,
codecs, 64-bit ID encoding, handles, and the unimplemented list.

See `docs/protocol.md` for framing, lifecycle, and extension rules.

## Scenarios

Scenarios are written in a Ruby DSL (`ruby/tinyactors_test.rb`). Charts live
in `ruby/charts` and scenarios in `ruby/scenarios`:

```ruby
TinyactorsTest.scenarios 'traffic light', chart: :traffic_light do
  invariant('exactly one light is on') { |light| (light.active & %i[red green yellow]).size == 1 }

  expect_failure 'core does not implement ta_system_create yet', rpc_error: -32_001, rpc_method: 'ta.system.create'
  scenario 'cycles on logical time' do
    light = spawn
    advance_time 30_000
    assert_active light, :green
  end
end
```

Each scenario runs against a fresh host process and drives the generated
`ta.*` bindings of the public C API (see "Scenario runner" in
`docs/protocol.md`). The steps are `spawn`, `advance_time` (in logical
milliseconds), `run_to_completion` (advance from one delayed-send deadline to
the next until an actor is done), and `actor.deliver`. The assertions
are `assert`, `assert_equal`, `assert_active`, `refute_active`, and
`assert_configuration`. `actor.data(filter)`, `actor.done?`, and
`actor.completion` observe an actor. Invariants are checked after every step.

Charts cover SCXML's structure and executable content: `state`, `parallel`,
`final`, `history`, `initial` (with actions), `transition`, `on_entry`,
`on_exit`, `data` (at the root or in a state), `invoke` (with a `finalize`
block), and `done_data`. The actions are `raise_event`, `send_event`,
`cancel`, `assign`, `log`, `script`, `conditional` (with `branch` and
`otherwise`), and `foreach`. See `ruby/tinyactors_test/chart.rb` for their
options. Each `on_entry` or `on_exit` call adds a separate handler, as in
SCXML: an error skips the rest of that handler, and the next handler still
runs.

`expect_failure` applies to the next scenario only, and it names the failure
it expects: `rpc_error:` (optionally with `rpc_method:`) and/or `message:`.

| Outcome | Meaning | Suite |
|---|---|---|
| PASS | Passed | passes |
| XFAIL | Failed as expected | passes |
| XPASS | Passed although a failure was expected; remove the marker | fails |
| FAIL | Any other failure, including a failure for a reason other than the expected one | fails |

Charts are checked when they are loaded (for unknown targets, duplicate IDs,
misplaced initial or history defaults, malformed SCXML durations, and literal
cancels with no matching `<send>` ID), so a typo in a chart cannot hide behind an expected failure.

```text
mise run scenarios                 # all scenarios
mise run scenarios -n stale        # scenarios whose "group: name" matches
```

The W3C SCXML conformance tests in `vendor/scxml-irp` are imported as the
`scxml-irp` scenarios (`mise run import-scxml-irp`), with their known failures
recorded in `ruby/scxml_irp/baseline.json` (`mise run scxml-irp-baseline`).
See `ruby/scxml_irp/README.md`.

`ruby/selftest.rb` verifies the DSL's outcome classification against the live
host. `ruby/runner_selftest.rb` runs the steps against a scripted fake host
whose answers stand in for core functions that are not implemented yet.

## Vendored dependency

The source release of jq 1.8.2 is stored in `vendor/jq` and statically linked
into the host. Run `mise run vendor-jq` at the repository root to reproduce the
vendor directory from its pinned release archive and SHA-256 checksum.
