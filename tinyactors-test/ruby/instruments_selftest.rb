# frozen_string_literal: true

# Verifies the test harness's own instruments against the live host: the
# instrumented adapters (src/instruments.c), the teardown leak check, the
# trace recorder, and assert_equivalent_runs. Each check is a scenario; the
# ones marked expect_failure prove that a check can fail.

require 'pathname'
require_relative 'tinyactors_test'

root = Pathname.new(__dir__).join('..').expand_path
Dir[root.join('ruby/charts/**/*.rb').to_s].sort.each { |file| require file }

SERVICE = 'urn:tinyactors:test:service'
IO_TYPE = 'urn:tinyactors:test:io'

# Sends itself `ping` through the SCXML Event I/O Processor, which routes
# external self-sends through the host's router.
TinyactorsTest.chart :harness_ping do
  data pings: 0
  data error: nil

  state :ready do
    transition on: 'go', type: :internal do
      send_event 'ping'
    end
    transition on: 'ping', type: :internal do
      assign '.pings', '.pings + 1'
    end
    transition on: 'error', target: :failed do
      assign '.error', '$_event.name'
    end
  end

  state :failed
end

# Calls a service that is not an actor.
TinyactorsTest.chart :harness_service do
  data reply: nil
  data error: nil

  state :calling do
    invoke type: SERVICE, id: :svc
    transition on: 'reply', target: :replied do
      assign '.reply', '$_event.data.value'
    end
    transition on: 'error', target: :failed do
      assign '.error', '$_event.name'
    end
  end

  state :replied do
    transition on: 'stop', target: :finished
  end

  state :failed
  final :finished
end

# Sends through a host I/O processor.
TinyactorsTest.chart :harness_io do
  data error: nil

  state :idle do
    transition on: 'go', type: :internal do
      send_event 'ping', type: IO_TYPE, to: 'somewhere', data: '{n: 1}'
    end
    transition on: 'error', target: :failed do
      assign '.error', '$_event.name'
    end
  end

  state :failed
end

# Logs a label with a value, a label alone, and a value alone; `bad` logs a
# failing expression.
TinyactorsTest.chart :harness_log do
  data n: 7

  state :ready do
    on_entry { log 'entry', expr: '.n' }
    transition on: 'go', type: :internal do
      log 'plain'
      log expr: '{a: .n}'
    end
    transition on: 'bad', type: :internal do
      log 'never', expr: 'error("boom")'
    end
    transition on: 'error.execution', target: :failed
  end

  state :failed
end

def check(condition, message)
  raise "instruments self-test failed: #{message}" unless condition
end

allocator_group = TinyactorsTest.scenarios 'allocator' do
  expect_failure 'a compiled definition that is never registered leaks', message: /allocation\(s\) \(\d+ bytes\) live/
  scenario 'teardown reports memory the system did not release' do
    compiled = ta.definition.compile_spec(system: system_handle, spec: TinyactorsTest.charts[:harness_ping].to_spec,
                                          datamodel: { '$adapter' => 'datamodel.jq' }, allocator: allocator.adapter)
    assert_equal 'ok', compiled[:result]
  end

  scenario 'an injected allocation failure is err_nomem, and the system recovers' do
    system_handle
    allocator.arm_fault(after: 0, count: 1)
    response = ta.definition.compile_spec(system: system_handle, spec: TinyactorsTest.charts[:harness_ping].to_spec,
                                          datamodel: { '$adapter' => 'datamodel.jq' }, allocator: allocator.adapter)
    assert_equal 'err_nomem', response[:result]
    assert_equal 1, allocator.stats[:failures]
    assert_equal 'alloc_failed', allocator.journal.last[:op]
    actor = spawn :harness_ping
    actor.deliver 'go'
    assert_equal 1, actor.data('.pings')
  end

  scenario 'a byte budget refuses what does not fit and is lifted again' do
    actor = spawn :harness_ping
    live = allocator.stats[:live_bytes]

    allocator.budget(live + 16)
    assert_equal live + 16, allocator.stats[:budget_bytes]
    response = ta.definition.compile_spec(system: system_handle, spec: TinyactorsTest.charts[:harness_ping].to_spec,
                                          datamodel: { '$adapter' => 'datamodel.jq' }, allocator: allocator.adapter)
    assert_equal 'err_nomem', response[:result]
    refused = allocator.journal.last
    assert_equal 'alloc_failed', refused[:op]
    assert_equal 'budget', refused[:reason]
    assert refused[:size] > 16, 'the refused allocation did not fit in the budget'

    allocator.clear_faults
    assert_equal 0, allocator.stats[:budget_bytes]
    actor.deliver 'go'
    assert_equal 1, actor.data('.pings'), 'the system runs again once the budget is lifted'
  end
end

inspector_group = TinyactorsTest.scenarios 'inspector' do
  scenario 'the recording inspector journals every visit and can stop the walk' do
    inspector = recording_inspector
    first = spawn :harness_ping
    second = spawn :harness_ping

    assert_equal 'ok', visit_actors
    assert_equal [first.handle, second.handle].sort, inspector.visits.map { |entry| entry[:actor] }.sort
    assert_equal 2, inspector.control('stats')[:visits]
    view = inspector.visits.first.fetch(:view)
    assert_equal 'idle', view[:residency], 'a quiescent actor is idle'
    assert_equal first.inspect_actor, view, 'the view is the one ta_actor_inspect answers with'

    # Armed before the walk it did not take part in yet: the instrument is
    # created by the control call itself.
    inspector.arm_fault(after: 0, result: 'err_not_found')
    assert_equal 'err_not_found', visit_actors
    assert_equal 3, inspector.visits.size, 'the failing visit stopped the walk'
  end
end

router_group = TinyactorsTest.scenarios 'router' do
  scenario 'the recording router journals external sends and delivers them' do
    router = recording_router
    actor = spawn :harness_ping
    actor.deliver 'go'
    assert_equal 1, actor.data('.pings')
    # The self-send is submitted while its sender is still in its macrostep,
    # so ta_actor_deliver answers busy, the router retries, and core offers
    # it again once the actor is stable.
    submits = router.entries('submit')
    assert_equal [%w[retry err_busy], %w[accepted ok]], submits.map { |entry| entry.values_at(:status, :result) }
    assert_equal %w[ping ping], submits.map { |entry| entry.dig(:event, :name_text) }
    assert_equal({ kind: 'runtime', runtime: actor.handle }, submits[0][:destination])
    assert_equal({ kind: 'actor', actor: { kind: 'runtime', runtime: actor.handle } }, submits[0].dig(:event, :origin))
  end

  scenario 'held deliveries arrive only when released, in the order chosen' do
    router = recording_router(mode: :hold)
    actor = spawn :harness_ping
    actor.deliver 'go'
    actor.deliver 'go'
    assert_equal 0, actor.data('.pings')
    held = router.held
    assert_equal 2, held.size
    assert_equal 'ok', router.release(held[1][:id])[:result]
    run_system('release')
    assert_equal 1, actor.data('.pings')
    router.drop(held[0][:id])
    assert_equal [], router.held
    assert_equal 1, actor.data('.pings')
  end

  scenario 'an injected retry makes core offer the delivery again' do
    router = recording_router
    router.arm_fault(:submit, status: 'retry')
    actor = spawn :harness_ping
    actor.deliver 'go'
    assert_equal 1, actor.data('.pings')
    assert_equal [%w[retry true], %w[accepted false]],
                 router.entries('submit').map { |entry| [entry[:status], (entry[:injected] || false).to_s] }
  end

  scenario 'a named destination resolves through the directory, which can fail' do
    directory = memory_directory
    router = recording_router(directory: true)
    sender = spawn :messenger, binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger
    directory.put(family: 'messenger', name: 'm2', kind: :resident, actor: receiver.handle)

    sender.deliver 'greet'
    assert_equal [{ event: 'hello', from: "#_scxml_#{sender.handle}" }], receiver.data('.inbox')
    # The router resolves through the directory's own operation, so the
    # directory journals it, rather than reading its entries behind its back.
    assert_equal [['messenger', 'm2', 'ok']],
                 directory.entries('resolve').map { |entry| entry.values_at(:family, :name, :result) }

    # A fault armed on resolve therefore reaches routing too.
    directory.arm_fault(:resolve, result: 'err_stale')
    sender.deliver 'greet'
    assert_equal 'err_stale', router.entries('submit').last[:result]
    assert_equal 1, receiver.data('.inbox').size, 'nothing was delivered while resolve failed'
    assert_equal %w[ok err_stale],
                 directory.entries('resolve').map { |entry| entry[:result] }
  end

  scenario 'an injected router error reaches the chart as error.communication' do
    router = recording_router
    router.arm_fault(:submit, message: 'link down')
    actor = spawn :harness_ping
    actor.deliver 'go'
    assert_active actor, :failed
    assert_equal 'error.communication', actor.data('.error')
  end
end

invoker_group = TinyactorsTest.scenarios 'invoker' do
  scenario 'a scripted service starts, reports an event, and is cancelled on exit' do
    service = scripted_invoker(SERVICE)
    actor = spawn :harness_service
    starts = service.started
    assert_equal 1, starts.size
    assert_equal actor.handle, starts[0][:parent]
    assert_equal SERVICE, starts[0][:type]
    report_invoke starts[0], :event, event: 'reply', data: { value: 42 }
    assert_active actor, :replied
    assert_equal 42, actor.data('.reply')
    cancels = service.entries('cancel')
    assert_equal [[starts[0][:service], true]], cancels.map { |entry| [entry[:service], entry[:known]] }
  end

  scenario 'a lost service reaches the chart as error.communication' do
    service = scripted_invoker(SERVICE)
    actor = spawn :harness_service
    report_invoke service.started.first, :unavailable
    assert_active actor, :failed
    assert_equal 'error.communication.invoke.svc.unavailable', actor.data('.error')
  end

  scenario 'an injected start failure reaches the chart as error.execution' do
    service = scripted_invoker(SERVICE)
    service.arm_fault(:start, message: 'no capacity')
    actor = spawn :harness_service
    assert_active actor, :failed
    assert_equal 'error.execution', actor.data('.error')
    assert_equal [true], service.started.map { |entry| entry[:injected] }
  end
end

log_group = TinyactorsTest.scenarios 'log' do
  scenario 'the log sink records label, value, and who logged it' do
    sink = recording_log
    actor = spawn :harness_log
    actor.deliver 'go'
    assert_equal [['entry', 7], ['plain', nil], ['', { a: 7 }]], sink.logged
    assert_equal [{ kind: 'runtime', runtime: actor.handle }],
                 sink.entries('log').map { |entry| entry[:source] }.uniq
    assert_equal [true, false, true], sink.entries('log').map { |entry| entry[:has_value] }
  end

  scenario 'a failing log expression raises an error and records nothing' do
    sink = recording_log
    actor = spawn :harness_log
    actor.deliver 'bad'
    assert_active actor, :failed
    assert_equal [['entry', 7]], sink.logged, 'only the entry log was recorded'
  end
end

io_group = TinyactorsTest.scenarios 'io' do
  scenario 'the recording I/O processor journals sends' do
    io = recording_io(IO_TYPE)
    actor = spawn :harness_io
    actor.deliver 'go'
    sends = io.entries('send')
    assert_equal 1, sends.size
    assert_equal ['ping', 'somewhere', { n: 1 }, IO_TYPE],
                 sends[0].values_at(:event_text, :target, :data, :io_type)
    assert_equal({ kind: 'runtime', runtime: actor.handle }, sends[0][:source])
  end

  scenario 'an injected send failure reaches the chart as error.communication' do
    io = recording_io(IO_TYPE)
    io.arm_fault(:send)
    actor = spawn :harness_io
    actor.deliver 'go'
    assert_active actor, :failed
    assert_equal 'error.communication', actor.data('.error')
  end
end

directory_group = TinyactorsTest.scenarios 'directory' do
  # Core does not call the directory yet (named actors are unimplemented), so
  # only the host-side store is exercised here.
  scenario 'the memory directory is installed and stores entries' do
    directory = memory_directory
    directory.put(family: 'door', name: 'd1', kind: :resident, actor: 7)
    directory.put(family: 'door', name: 'd2', kind: :unloaded)
    assert_equal [{ family: 'door', name: 'd1', kind: 'resident', actor: 7 },
                  { family: 'door', name: 'd2', kind: 'unloaded' }],
                 directory.directory_entries.sort_by { |entry| entry[:name] }
    directory.put(family: 'door', name: 'd1', kind: :none)
    assert_equal 1, directory.directory_entries.size
  end
end

LIGHT_BODY = proc do
  light = spawn
  advance_time 30_000
  light.deliver 'fault'
  advance_time 5_000
  light.deliver 'repair'
  advance_time 30_000
end

# Delivers `fault` to the first resident actor, behind the scenario's back.
STRAY_FAULT = lambda do |context|
  id = context.rpc('system.actors', system: context.system_handle).first
  event = { name: context.event_name('fault'), type: 'external', invoke_id: TinyactorsTest::INVOKE_ID_NONE }
  context.core('ta_actor_deliver', context.ta.actor.deliver(system: context.system_handle, actor: id, event: event))
end

equivalence_group = TinyactorsTest.scenarios 'equivalence', chart: :traffic_light do
  scenario 'pausing after every microstep and dispatching one send at a time leave the trace unchanged' do
    trace = assert_equivalent_runs(variants: { 'resume budget 1' => { resume_budget: 1 },
                                               'dispatch budget 1' => { dispatch_budget: 1 } }, &LIGHT_BODY)
    assert_equal ['spawn traffic_light', 'advance_time 30000 (t=30000ms)', 'deliver fault',
                  'advance_time 5000 (t=35000ms)', 'deliver repair', 'advance_time 30000 (t=65000ms)'],
                 trace.steps.map(&:label)
    first = trace.steps[0].state[:actors].fetch('traffic_light.1')
    assert_equal %i[operating red], first[:active]
    assert_equal [{ due: 30_000, event: 'timer' }], first[:delayed_sends].map { |send| send.slice(:due, :event) }
    assert_equal %i[operating green], trace.steps[1].state[:actors].fetch('traffic_light.1')[:active]
  end

  scenario 'an interruption that changes nothing observable passes at every step' do
    reinstall = lambda do |context|
      router = { '$adapter' => 'router.direct', 'config' => { 'system' => context.system_handle } }
      context.core('ta_system_set_external_event_router',
                   context.ta.system.set_external_event_router(system: context.system_handle, router: router))
    end
    assert_equivalent_runs(interruptions: { 'reinstall the router' => reinstall }, &LIGHT_BODY)
  end

  expect_failure 'a stray event after the first step changes the trace', message: /diverges from baseline at step 0 \(spawn traffic_light/
  scenario 'an interruption that changes behaviour is reported at the step it follows' do
    assert_equivalent_runs(interruptions: { 'stray fault' => STRAY_FAULT }, &LIGHT_BODY)
  end

  scenario 'journals appear in the trace with actor labels instead of runtime IDs' do
    trace = assert_equivalent_runs(variants: { 'resume budget 1' => { resume_budget: 1 } }) do
      recording_router
      spawn(:harness_ping).deliver 'go'
    end
    submit = trace.steps.last.state[:journals].fetch('router').first
    assert_equal({ kind: 'runtime', runtime: 'harness_ping.1' }, submit[:destination])
    assert !submit.key?(:seq), 'journal seq numbers are not part of the trace'
  end
end

groups = [allocator_group, inspector_group, router_group, invoker_group, io_group, log_group, directory_group, equivalence_group]
runner = TinyactorsTest::Runner.new(executable: root.join('build/tinyactors-test'), charts: TinyactorsTest.charts)
results = runner.run(groups)
failed = results.reject(&:ok?).map { |result| result.scenario.full_name }
check(failed.empty?, "#{failed.size} check(s) failed: #{failed.join('; ')}")
check(results.count { |result| result.outcome == :xfail } == 2, 'both deliberate failures were detected')
puts 'tinyactors-test instruments self-test passed'
