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
tinyactors-core/include/tinyactors.h           (annotated with TA_* macros)
  -> mise run api        (core: clang, annotations, layouts, schema, compile check)
  -> tinyactors-core/include/tinyactors.api.json   (checked in; the only file between the steps)
  -> mise run bindings   (this project: mise/lib/rpc + bindings.rb, in memory)
  -> build/gen/ta_rpc.{c,h}                        (generated, not checked in)
```

`mise run build` runs both steps. The generator reads nothing about the C API
except `tinyactors.api.json` (see `tinyactors-core/docs/internal/api-ir.md`):
pointer shapes, nullability, output validity, releases, handles, enum symbols,
and the data model of `ta_value` inputs all come from it. `mise/lib/rpc` works
in three stages. `Rpc::Api` reads the description and checks its format.
`Rpc::Plan` makes every binding decision: members, codecs, required
parameters, union layouts, outputs, releases, and value receivers.
`Rpc::Emit` formats the plan as C without deciding anything.

The generator emits a JSON decoder and encoder for every bound type, a
`codec.roundtrip.<type>` method per struct, and one `ta.<domain>.<rest>`
method per public function. A function that core does not implement yet is
still bound: its method validates params and then answers error -32001, and
discovery lists it under `unimplemented`.

`bindings.rb` is the binding policy: what the description cannot decide for
this host. It lists the functions and types that are skipped or implemented by
hand, each with a reason; the union members that are flattened into their
parent object; the value-model kinds the host cannot resolve; and the largest
caller buffer a request may ask for. The generator fails when an item fits no
decision, when a bound item needs a skipped one, when a flattened member's
keys collide, and when a policy entry matches nothing, so new core API cannot
silently go unbound and the policy cannot go stale. `ruby/bindings_generator_test.rb`
tests these refusals. Hand-written parts live in `src/binding.{h,c}` (request
arena, handle table, primitive codecs, the codecs of the string, bytes, and
host value types, and the `ta_expression` override) and `src/adapters.c` (the
`$adapter` registry). The generator also writes `build/gen/coverage.json` with
the bound, skipped, and unimplemented counts.

See `docs/protocol.md` for method naming, result shapes, value encoding,
handles, and adapters.

The host includes three data models, chosen per chart by its `datamodel`:
`datamodel.jq` (below), `datamodel.ecmascript`, backed by quickjs-ng (see
`docs/ecmascript-datamodel.md`), and `datamodel.null`, the minimum SCXML B.1
profile (`src/null_datamodel.c`): conditions are the `In(state)` predicate
and nothing else, there are no data bindings, values or scripts, and an
actor of such a chart carries no data-model instance. The two are fully separate; values cross
between them, and over the protocol, only as JSON through each model's value
codec.

In the jq data model, source expressions and scripts
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
in `ruby/charts` and scenarios in `ruby/scenarios`.

**Before adding scenarios, read
[`tinyactors-core/docs/internal/testing-and-api-change.md`](../tinyactors-core/docs/internal/testing-and-api-change.md).**
It describes how to write a scenario that can actually catch a bug, the
traps that make tests look like they do when they don't, and when a failure
means the public API should change instead of the test. This page documents
the machinery; that page documents the method.

A scenario group looks like this:

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
are `assert`, `assert_equal`, `assert_active`, `refute_active`,
`assert_configuration`, `assert_logs`, and `assert_logged`.
`actor.data(filter)`, `actor.done?`, and `actor.completion` observe an
actor. Invariants are checked after every step.

Lower-level steps control a macrostep directly:

- `actor.post(event)` offers an event without running the system, and
  returns the `ta_actor_deliver` result (`"ok"`, `"err_busy"`, ...).
- `actor.resume(budget:)` makes one `ta_actor_resume` call and returns its
  status (`"paused"`, `"stable"`, `"done"`).
- `actor.destroy` destroys the actor and runs the system.

Further observations:

- `logs` is what the charts logged since `recording_log` installed the sink:
  `[{label:, value:, actor:}]`. `assert_logs(['phase', 'start'], ...)` states
  the records in order, `assert_logged(label, value, actor:, times:)` one of
  them.
- `visit_actors` walks the resident actors with `recording_inspector`, whose
  `visits` are the views core handed the callback (`ta_system_visit_actors`).
- `parse_target(text)` and `format_target(target, capacity:)` are core's
  SCXML target URI grammar (ADR 0044), the parser and the writer that a
  `<send>` target and `$_event.origin` both go through.
- `actor.resident?` tells whether the actor still exists.
- `actor.history(:id)` lists the states a history pseudo-state recorded.
- `actor.sends` lists the pending delayed sends.
- `actor.observe` returns everything a trace records about the actor.
- `next_deadline` is the earliest pending deadline, or nil when none is pending.
- `definition(chart)` is a chart's registered definition ID.

A definition is the host's until it registers it (ADR 0043):
`compile_only(chart)` compiles one without registering, `destroy_definition`
releases it, `register_definition` hands it to the system, and
`retire_definition` ends its availability.

Charts cover SCXML's structure and executable content: `state`, `parallel`,
`final`, `history`, `initial` (with actions), `transition`, `on_entry`,
`on_exit`, `data` (at the root or in a state), `invoke` (with a `finalize`
block), and `done_data`. The actions are `raise_event`, `send_event`,
`cancel`, `assign`, `log`, `script`, `conditional` (with `branch` and
`otherwise`), and `foreach`. Platform-specific executable content is a
`<send>` to a host I/O processor (ADR 0046); `ruby/scenarios/extensions.rb`
shows the pattern. See `ruby/tinyactors_test/chart.rb` for their options. Each `on_entry` or `on_exit` call adds a separate handler, as in
SCXML: an error skips the rest of that handler, and the next handler still
runs.

Each scenario's system uses a counting allocator (`allocator.counting`).
After the scenario, the runner destroys the system. The scenario fails when
an allocation is still live, or when core freed memory it was not given.

### Session images and named actors

- `spawn(chart, name: 'x')` spawns a named actor at the address (the
  chart's definition family, `'x'`), and installs a `memory_directory` on
  first use. `spawn(chart, binding: {...})` gives the actor a host
  data-model binding, as an invoked child's `<param>`s would.
- A chart sends to a logical address with the target
  `#_actor_<family>/<name>`, which core parses (ADR 0044);
  `recording_router(directory: true)` resolves those destinations through
  the directory, because the host owns location and residency.
- `capture(actor)` returns an image handle. `image_deadline(image)` gives
  the image's earliest deadline, and `destroy_image(image)` frees it.
- `reconstruct(image, binding:)` rebuilds an anonymous session.
- `unload(actor, image)`, `load(actor, image, binding:)`,
  `find(address)`, and `remove_unloaded(address)` drive residency.
  `load` points the Actor at its new runtime ID.
- `prepare_replacement(incumbent, chart, binding:)` and
  `activate_replacement(incumbent, replacement)` upgrade a named actor.
- `reconstruct_in_place(actor)` and `cycle_residency(actor)` perform a whole
  round trip with the actor's current data root as the binding. They are
  meant as `assert_equivalent_runs` interruptions.

Core claims, resolves and releases addresses, so `spawn name:`, `find` and a
named actor's origin work against a real core. It does not implement session
images yet, so `capture`, `unload`, `load`, `remove_unloaded` and the two
replacement helpers still answer -32001: the scenarios in
`ruby/scenarios/residency.rb` and the residency half of
`ruby/scenarios/named_actors.rb` are expected failures that name the
unimplemented call. `ruby/runner_selftest.rb` runs the helpers against a fake
host.

### Instruments

Scenarios that need more than the default adapters install instruments,
which journal every call core makes to them and fail on command (see
"Instruments" in `docs/protocol.md`):

```ruby
router  = recording_router(mode: :hold)   # replaces router.direct
service = scripted_invoker('urn:example:service')
io      = recording_io('urn:example:io')
sink    = recording_log                    # what the charts <log>
walk    = recording_inspector              # the ta_actor_inspector of a walk
dir     = memory_directory
allocator                                  # the system's counting allocator

router.held; router.release(id); router.drop(id)
router.arm_fault(:submit, status: 'retry')
report_invoke service.started.first, :event, event: 'reply', data: { value: 1 }
report_invoke service.started.first, :unavailable
allocator.arm_fault(after: 3, count: 1); allocator.stats
allocator.budget(4096)                     # refuse what does not fit (ADR 0023)
walk.arm_fault(after: 1, result: 'err_busy')
io.entries('send')
sink.logged                                # [[label, value], ...]
```

### Traces and equivalent runs

`assert_equivalent_runs` runs a scenario body several times, each run
against its own system. After every step it records a trace of everything
it can observe. For every resident actor, invoked children included, that
is the configuration, residency, data-model root, completion, delayed
sends, and invocations. It also records the new journal entries of every
instrument. The runs must produce the same trace as the baseline run, step by step:

```ruby
assert_equivalent_runs(
  variants:      { 'paused' => { resume_budget: 1 }, 'one send per dispatch' => { dispatch_budget: 1 } },
  interruptions: { 'reinstall router' => ->(context) { ... } }  # tried after each baseline step
) do
  light = spawn
  advance_time 30_000
  light.deliver 'fault'
end
```

`variants` are Context options. `interruptions` run once for each step of
the baseline, after that step only. `normalize:` removes differences that a
claim allows. On a mismatch, the error names the first step that differs and
the path of the first difference. Actors appear in traces under labels: a
spawned actor as `<chart>.<n>`, any other as `actor.<n>`. So runtime IDs do
not count as differences, and `rebind(actor, new_id)` keeps an actor's label
after a reload.

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

`ruby/export_roundtrip_check.rb` compiles every chart the suite defines,
exports it with `ta.definition.export`, recompiles the export, and checks that
the revision is unchanged and the export matches the chart's spec (ADR 0042).
Charts whose data model has no adapter in the host are skipped.

`ruby/selftest.rb` verifies the DSL's outcome classification against the live
host. `ruby/runner_selftest.rb` runs the steps against a scripted fake host
whose answers stand in for core functions that are not implemented yet.
`ruby/instruments_selftest.rb` checks each instrument, the leak check, and
`assert_equivalent_runs` against the live host, including that a leak and a
divergent run are detected.

## Vendored dependencies

The source release of jq 1.8.2 is stored in `vendor/jq` and statically linked
into the host. Run `mise run vendor-jq` at the repository root to reproduce the
vendor directory from its pinned release archive and SHA-256 checksum.

The source release of quickjs-ng 0.17.0 is stored in `vendor/quickjs-ng` and
statically linked into the host as `build/prefix/lib/libqjs.a`, with
`quickjs.h` installed into `build/prefix/include`. Run `mise run vendor-quickjs`
at the repository root to reproduce the vendor directory from its pinned
release archive and SHA-256 checksum. The build compiles the library only when
`libqjs.a` or `quickjs.h` is missing from `build/prefix`; delete them to force a
rebuild.
