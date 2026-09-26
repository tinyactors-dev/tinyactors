# tinyactors-test

`tinyactors-test` is the instrumentable host used to verify Tinyactors. It
links the production `tinyactors-core` amalgamation and exposes a persistent
JSON-RPC 2.0 subset over newline-delimited JSON on standard input and output.
Human-readable diagnostics are written to standard error.

The Ruby driver owns scenario descriptions, scheduling choices, assertions,
generation, shrinking, and reports. Scheduling is one loop,
`TinyactorsTest::Scheduler` (`ruby/tinyactors_test/scheduler.rb`), which the
scenarios, the workbench and the debug adapter share; a debugger steers it
by holding actors out of it, stopping it between two resumes, and choosing
the budgets of each resume. The C host owns Tinyactors objects, test
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
in three stages. `RPC::API` reads the description and checks its format.
`RPC::Plan` makes every binding decision: members, codecs, required
parameters, union layouts, outputs, releases, and value receivers.
`RPC::Emit` formats the plan as C without deciding anything.

The generator emits a JSON decoder and encoder for every bound type, a
`codec.roundtrip.<type>` method per struct, and one `ta.<domain>.<rest>`
method per public function. A function that core does not implement yet is
still bound: its method validates params and then answers error -32001, and
discovery lists it under `unimplemented`. Core implements every bound
function today, so that list is empty and no method answers -32001; the
machinery is what keeps a newly declared function honest until it is written.

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
the bound, overridden, skipped, and unimplemented counts for functions and
types, the method total, and the reason recorded against every skipped item.

See `docs/protocol.md` for method naming, result shapes, value encoding,
handles, and adapters.

The host includes three data models, chosen per chart by its `datamodel`:
`datamodel.jq` (below), `datamodel.ecmascript`, backed by quickjs-ng (see
`docs/ecmascript-datamodel.md`), and `datamodel.null`, the minimum SCXML B.1
profile, which core ships as `ta_null_datamodel()` (ADR 0057): conditions are the `In(state)` predicate
and nothing else, there are no data bindings, values or scripts, and an
actor of such a chart carries no data-model instance. The three are fully
separate; values cross between them, and over the protocol, only as JSON
through each model's value codec. `rpc.discover` lists them under
`datamodels`.

In the jq data model, source expressions and scripts
are jq filters over the actor's JSON root. A condition uses jq truthiness, an
assignment evaluates its expression as a jq location, and a script must produce
exactly one replacement root. Values use Tinyactors' normal retain and release
contract.

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

`mise run workbench` opens the interactive workbench (below); `mise run
scenarios` runs the Ruby scenarios alone.

## Population benchmark

`tests/scale_test.c` is what stands behind the four-million-actor target of
`tinyactors-core/docs/internal/memory-model.md`. It links core's release
archive and the null data model, and it measures three things per actor
against a live population: spawning and bootstrapping a named actor,
upgrading one to a new revision of its chart (`prepare_replacement`, run,
`activate_replacement`, destroy the incumbent), and an ephemeral actor's
whole spawn/bootstrap/destroy life.

`mise run test` runs it as a test of invariant 17: it takes each measurement
at 50,000, 200,000, 1,000,000 and 4,000,000 actors, and fails when the cost
of one actor grows from one population to the next, which is what an
actor-table walk inside a spawn looks like. Measuring up to the target takes
about 30 seconds and a peak of about 9.5 GB.

The test sizes itself to the machine it runs on. The first two populations
always run. Before each larger one, it predicts the memory that population
needs from the process's own peak so far, and runs it only when that fits
in three quarters of the memory available at that moment. Available memory
is read from the Mach VM statistics on macOS and from `MemAvailable` on
Linux. It prints its decision either way:

```text
measuring 4000000 actors: needs about 9.1 GB, 21.6 GB available
stopping at 1000000 actors: 4000000 would need about 9.1 GB, and 4.0 GB is available (three quarters of it may be used)
```

On a platform that reports neither, it stops after the first two
populations. `TAT_SCALE_MAX_POPULATION` overrides the sizing: every
population up to it runs, whatever memory the machine reports, and none
above it. `TAT_SCALE_MAX_POPULATION=200000` is the quick check, about a
second. It asserts no absolute time — that would only measure the
machine — and it measures CPU time, so a loaded machine slows it down
without changing the verdict. Per-actor memory is held to a budget in the
same way.

Give it a population to run as a benchmark instead, with no ratios and no
bounds:

```text
tinyactors-test/build/scale_test --report 4000000
```

```text
              actors      spawn  upgrade    churn     bytes    peak MB
                             us       us       us    /actor
report       4000000       0.36     0.39     0.16     557.5     2126.7
```

That is an Apple M-series laptop: four million resident named actors in
2.1 GB of core memory, and a rolling upgrade of all four million in 1.6 s of
CPU time. The bytes are what core asked the allocator for; a real host adds
its own directory, its names, and whatever its data model keeps per actor.

## Host methods

Ten methods are written by hand rather than generated:

- `rpc.discover`
- `system.ping`
- `json.transform`
- `metrics.snapshot`
- `host.shutdown`
- `system.actors` (resident actor IDs; `"runnable": true` lists only those with a macrostep in progress)
- `handle.fixture`
- `instrument.list`, `instrument.journal`, and `instrument.control`, the
  instrument protocol (see "Instruments"); `TinyactorsTest::Instrument` is a
  wrapper over the last two.

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
`assert_configuration`, `assert_logs`, `assert_logged`, and the pair
`assert_equivalent_runs` / `assert_runs_diverge` (below).
`actor.data(filter)`, `actor.done?`, and `actor.completion` observe an
actor. Invariants are checked after every step.

Lower-level steps control a macrostep directly:

- `actor.post(event)` offers an event without running the system, and
  returns the `ta_actor_deliver` result (`"ok"`, `"err_busy"`, ...).
- `actor.resume(budget:)` makes one `ta_actor_resume` call and returns its
  status (`"paused"`, `"stable"`, `"done"`).
- `actor.destroy` destroys the actor and runs the system.
- `actor.jump(*states, from:)` delivers an event (`operator.goto` unless
  `event:` names another) with an operator transition to those states, from
  the active state `from` or the root, and runs the system (core ADR 0082).
  `actor.post_jump` offers the same without running the system and returns
  the `ta_actor_deliver` result.
- `actor.abandon` abandons the actor's macrostep, without running the
  system, and returns the `ta_actor_abandon` result (core ADR 0082).

Further observations:

- `logs` is what the charts logged since `recording_log` installed the sink:
  `[{label:, value:, actor:}]`. `assert_logs(['phase', 'start'], ...)` states
  the records in order, `assert_logged(label, value, actor:, times:)` one of
  them.
- `visit_actors(inspector, from:, limit:)` walks the resident actors with
  `recording_inspector` and answers `[result, next]`: everything by default,
  or one page from `from`. The inspector's `visits` are the views core
  handed the callback (`ta_system_visit_actors`, ADR 0062).
- `parse_target(text)` and `format_target(target, capacity:)` are core's
  SCXML target URI grammar (ADR 0044), the parser and the writer that a
  `<send>` target and `$_event.origin` both go through.
- `actor.resident?` tells whether the actor still exists.
- `actor.history(:id)` lists the states a history pseudo-state recorded.
- `actor.sends` lists the pending delayed sends.
- `actor.observe` returns everything a trace records about the actor.
- `next_deadline` is the earliest pending deadline, or nil when none is pending.
- `system_time` is the logical time core reports, which `now` mirrors.
- `definition(chart)` is a chart's registered definition ID,
  `definition_facts(chart)` its `ta_definition_inspect` identity and counts,
  and `export(chart)` its `ta_definition_export` spec.
- `actors` are the actors the scenario spawned and `invoked_actors` the
  resident ones it did not; `address(chart, name)` and `family(chart)` build
  and read a logical address.
- `rpc(method, params)` and `ta` are the escape hatches: the raw JSON-RPC
  call and the generated binding proxy, for anything the DSL does not wrap.

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
  `#_actor_<family>/<name>`, which core parses (ADR 0044). The default
  `router.direct` resolves such a destination with `ta_actor_find()`, so the
  directory a named spawn already installs is enough for one actor to address
  another. `recording_router(directory: true)` resolves them through the
  directory instrument instead, which journals the resolution and can fault
  it; a host needs its own router when it owns location, not merely naming.
- `capture(actor)` returns an image handle. `image_deadline(image)` gives
  the image's earliest deadline, and `destroy_image(image)` frees it.
- Reconstruction has no wrapper: a scenario calls
  `ta.actor.reconstruct(...)` through `rpc`/`core` directly, as
  `ruby/scenarios/residency.rb` does.
- `unload(actor, image)`, `load(actor, image, binding:)`,
  `find(address)`, and `remove_unloaded(address)` drive residency.
  `load` points the Actor at its new runtime ID.
- `prepare_replacement(incumbent, chart, binding:)` and
  `activate_replacement(incumbent, replacement)` upgrade a named actor.
- `cycle_residency(actor)` performs a whole capture/unload/load round trip
  with the actor's current data root as the binding. It is meant as an
  `assert_equivalent_runs` interruption.
- `forget(actor)` stops invariants being checked against an actor the
  scenario destroyed, and `rebind(actor, id)` moves one onto a new runtime ID.

Core implements all of this against a real core: it claims, resolves and
releases addresses, and implements `capture`, `reconstruct` and the image
readers, the residency protocol (`unload`, `load`, `remove_unloaded`), and the
definition upgrade of a named actor (`prepare_replacement`,
`activate_replacement`). `ruby/scenarios/named_actors.rb` drives all of it
without an `expect_failure`. `ruby/runner_selftest.rb` runs the helpers against
a scripted fake host, which is faster than a real run and still checks every
argument name against the real host's `rpc.discover` schemas.

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
report_invoke service.started.first, :error, reason: 'unavailable', ends: true
allocator.arm_fault(after: 3, count: 1); allocator.stats
allocator.budget(4096)                     # refuse what does not fit (ADR 0023)
walk.arm_fault(after: 1, result: 'err_busy')
io.entries('send')
sink.logged                                # [[label, value], ...]

router.mode(:direct)                       # change mode after installing it
dir.put(family: 'light', name: 'l1', kind: :resident, actor: id)
dir.directory_entries                      # what the directory holds
any.journal(since: seq)                    # entries after one seq
any.clear_faults                           # disarm what arm_fault armed
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
it expects: `rpc_error:` (optionally with `rpc_method:`) and/or `message:`. No
scenario in `ruby/scenarios` uses it: core implements the whole bound API, so
a scenario that fails there is a bug rather than a known gap. The imported
SCXML IRP suite is where it earns its keep, against its recorded baseline.

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
that answers as a fully implemented core would, so the DSL's own bookkeeping
is checked without paying for a real run.
`ruby/instruments_selftest.rb` checks each instrument, the leak check, and
`assert_equivalent_runs` against the live host, including that a leak and a
divergent run are detected.

## The interactive workbench

`bin/workbench` is a live system you can watch and drive, whose charts are
files on disk. Saving a chart upgrades the running actors to it in place: the
host process, the system, the actor addresses, and the accumulated data-model
state all survive, because every top-level actor is a named actor and the
reload goes through `ta_actor_prepare_replacement` and
`ta_actor_activate_replacement`.

```text
mise run build
mise run workbench            # opens ./workbench in tmux
```

It drives a `TinyactorsTest::Context`, the same class the scenario suite uses,
so it exercises the public C API through the generated bindings. A tmux window
shows the resident actors, the focused actor's data model, and the log; a
command pane delivers events and advances logical time.

See `ruby/workbench/README.md` for the directory format, the reload semantics,
and the commands. `ruby/workbench_selftest.rb` checks it against the live host.

`bin/dap` (`mise run dap`) is a debug adapter for the same directories: any
Debug Adapter Protocol client (VS Code, nvim-dap, Emacs dape) can launch a
workbench, stop and step its actors on `action_budget: 1`, set line
breakpoints in the chart files, look at their data, configuration, queues
and timers, send them events, and fork an actor into a system of its own
to step back through its future. See
`ruby/dap/README.md`; `ruby/dap_selftest.rb` checks it over pipes, and
`ruby/dap_transcripts.rb` plays the DAP transcripts it shares with napi's
adapter (`dap/README.md`).

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
