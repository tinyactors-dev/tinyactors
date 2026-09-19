# frozen_string_literal: true

# Session images (ADR 0038, ADR 0039, ADR 0026): capture records the whole
# interpreter session without changing it; reconstruction against the exact
# definition revision and the host's data-model binding continues it as if
# nothing happened, except that invocations are lost and reported as
# error.communication.invoke.<id>.unavailable.
#
# Core does not implement capture yet. Every scenario is marked with the
# failure that proves it: an RPC -32001 from an unimplemented function. Once
# core implements them, a scenario that passes becomes an XPASS (remove its
# marker) and one that fails differently becomes a FAIL (a bug in core or in
# the scenario). The code after the first unimplemented call has not run
# against a real core yet; runner_selftest.rb checks the helpers it uses.
module ResidencyScenarios
  UNIMPLEMENTED = /: -32001 /
end

TinyactorsTest.scenarios 'residency', chart: :traffic_light do
  # Catches: capture that mutates the actor (moves its timers out of the
  # heap, clears its queue, advances its epoch observably).
  expect_failure 'ta_actor_capture is unimplemented', message: ResidencyScenarios::UNIMPLEMENTED
  scenario 'capturing an actor at any step changes nothing' do
    capture_only = ->(context) { context.actors.each { |actor| context.destroy_image(context.capture(actor)) } }
    assert_equivalent_runs(interruptions: { 'capture' => capture_only }) do
      light = spawn
      relay = spawn :relay
      advance_time 30_000
      light.deliver 'fault'
      relay.deliver 'go'
      light.deliver 'repair'
      advance_time 25_000
    end
  end

  # Catches: an image that misses a timer (the light would stall), keeps a
  # send ID or due time only approximately, drops history (repair would go
  # to red), or loses data the binding does not carry.
  expect_failure 'ta_actor_capture is unimplemented', message: ResidencyScenarios::UNIMPLEMENTED
  scenario 'reconstructing an actor at any step is invisible' do
    round_trip = ->(context) { context.actors.each { |actor| context.reconstruct_in_place(actor) } }
    assert_equivalent_runs(interruptions: { 'capture, destroy, reconstruct' => round_trip }) do
      light = spawn
      relay = spawn :relay
      advance_time 30_000
      light.deliver 'fault'
      relay.deliver 'go'
      light.deliver 'repair'
      advance_time 25_000
    end
  end

  # Catches: an image of a paused actor that loses the continuation, the
  # current external event, or the internal queue, so the macrostep
  # restarts, repeats a microstep, or skips `counted`.
  expect_failure 'ta_actor_capture is unimplemented', message: ResidencyScenarios::UNIMPLEMENTED
  scenario 'a macrostep captured while paused continues where it paused' do
    router = recording_router(mode: :hold)
    relay = spawn :relay
    relay.post('go')
    3.times { assert_equal 'paused', relay.resume(budget: 1) }
    before = relay.observe
    assert_equal 2, before.dig(:data, :n)

    reconstruct_in_place(relay)
    assert_equal before, relay.observe, 'the paused session is restored exactly'
    assert_equal 'go', event_text(relay.inspect_actor.dig(:current_event, :name))
    assert_equal 'paused', relay.resume(budget: 1)
    assert_equal 3, relay.data('.n'), 'the next resume runs the next microstep'
    run_system('finish the macrostep')
    router.held.each { |delivery| router.release(delivery[:id]) }
    run_system('release the self-sent ping')
    assert_equal ['go', 'n=5', 'counted', 'ping'], relay.data('.log')
  end

  # Catches: reconstruction that restarts invocations (a duplicate external
  # effect), rebinds them silently, reports the loss in the wrong order or
  # with the wrong envelope, or still accepts events from a lost service.
  expect_failure 'ta_actor_capture is unimplemented', message: ResidencyScenarios::UNIMPLEMENTED
  scenario 'reconstruction reports each lost invocation once, in invocation order' do
    service = scripted_invoker('urn:tinyactors:test:service')
    pair = spawn :service_pair
    starts = service.started
    assert_equal 2, starts.size

    reconstruct_in_place(pair)
    run_system('reconstructed')
    assert_equal 2, service.started.size, 'reconstruction starts no service'
    assert_equal [{ name: 'error.communication.invoke.first.unavailable', type: 'platform', invokeid: nil },
                  { name: 'error.communication.invoke.second.unavailable', type: 'platform', invokeid: nil }],
                 pair.data('.errors')
    assert_equal [], pair.observe[:invokes], 'no invocation is active after reconstruction'

    lost = starts[0].merge(parent: pair.handle)
    report = ta.actor.report_invoke(system: system_handle, actor: lost[:parent], invoke_id: lost[:invoke_id],
                                    kind: 'event', event: { name: event_name('reply'), type: 'external',
                                                            invoke_id: lost[:invoke_id] })
    run_system("late reply from the lost service (#{report[:result]})")
    assert_equal [], pair.data('.replies'), 'events from a lost invocation are ignored'
  end

  # Catches: reconstruction against any definition of the same family (a
  # silent reinterpretation of state IDs), or an image that keeps its
  # retired definition alive.
  expect_failure 'ta_actor_capture is unimplemented', message: ResidencyScenarios::UNIMPLEMENTED
  scenario 'reconstruction needs the exact definition revision' do
    light = spawn
    advance_time 10_000
    image = capture(light)
    binding = light.data
    original = definition
    light.destroy
    ta.system.retire_definition(system: system_handle, definition: original)
    assert_equal 'err_not_found', ta.definition.inspect(system: system_handle, definition: original)[:result],
                 'an image does not keep its definition alive'

    spec = TinyactorsTest.charts[:traffic_light].to_spec
    changed = spec.merge(data: spec[:data] + [{ name: 'revision', initializer: { expr: '2' } }])
    register = lambda do |chart_spec|
      compiled = core('ta_definition_compile_spec',
                      ta.definition.compile_spec(system: system_handle, spec: chart_spec,
                                                 datamodel: { '$adapter' => 'datamodel.jq' }, allocator: allocator.adapter))
      core('ta_system_register_definition',
           ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out))).fetch(:out)
    end
    register.call(changed)
    response = ta.actor.reconstruct(system: system_handle, image: image, datamodel_binding: binding)
    assert_equal 'err_definition_mismatch', response[:result]

    exact = register.call(spec)
    restored = TinyactorsTest::Actor.new(self, reconstruct(image, binding: binding), :traffic_light, exact)
    destroy_image(image)
    assert_equal [{ due: 30_000, event: 'timer' }], restored.sends.map { |send| send.slice(:due, :event) }
    advance_time 20_000
    assert_equal %i[operating green], restored.active, 'the restored timer fires at its original due time'
    assert_equal({ cycles: 0 }, restored.data)
  end

  # Catches: an image deadline that is relative, stale, or missing, which a
  # host needs to wake an unloaded actor on time.
  expect_failure 'ta_actor_capture is unimplemented', message: ResidencyScenarios::UNIMPLEMENTED
  scenario 'an image reports its earliest delayed-send deadline' do
    light = spawn
    relay = spawn :relay
    advance_time 10_000
    light_image = capture(light)
    relay_image = capture(relay)
    assert_equal 30_000, image_deadline(light_image)
    assert_equal nil, image_deadline(relay_image), 'an actor without delayed sends has no deadline'
    destroy_image(light_image)
    destroy_image(relay_image)
  end
end
