# frozen_string_literal: true

# Session images (ADR 0038, ADR 0039, ADR 0026): capture records the whole
# interpreter session without changing it; reconstruction against the exact
# definition revision and the host's data-model binding continues it as if
# nothing happened, except that invocations are lost and reported as
# error.communication.invoke.<id>.unavailable.
#
# Core implements capture and reconstruction. One scenario remains an
# expected failure, for a reason that is no longer about an unimplemented
# function; its marker names it.
module ResidencyScenarios
end

TinyactorsTest.scenarios 'residency', chart: :traffic_light do
  # Catches: capture that mutates the actor (moves its timers out of the
  # heap, clears its queue, advances its epoch observably).
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
  # Everything this scenario was written to catch now holds: the macrostep
  # continues rather than restarting, `.n` resumes at 3 rather than repeating
  # a microstep, and `counted` survives in the internal queue. What is left is
  # the `ping`: the router is holding that delivery from before the capture,
  # addressed to the runtime ID of the session reconstruction replaced, and
  # core cannot re-address a delivery the host already holds. Whether an
  # anonymous reconstruction should instead keep its runtime ID — which is the
  # only identity such a session has, and so the only way it can be invisible
  # to a host holding one — is an open question; ta_actor_load() deliberately
  # does the opposite, and session-image.md calls the captured ID "local
  # freshness metadata, not portable identity".
  expect_failure 'a delivery the router already holds names the replaced session',
                 message: /expected \["go", "n=5", "counted", "ping"\], got \["go", "n=5", "counted"\]/
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

    # The invocation is retired, so reporting for it names no invocation.
    lost = starts[0]
    report = ta.actor.report_invoke(system: system_handle, actor: pair.handle, invoke_id: lost[:invoke_id],
                                    kind: 'event', event: { name: event_name('reply'), type: 'external',
                                                            invoke_id: lost[:invoke_id] })
    assert_equal 'err_not_found', report[:result]
    run_system('late reply from the lost service')
    assert_equal [], pair.data('.replies'), 'events from a lost invocation are ignored'
  end

  # Catches: reconstruction against any definition of the same family (a
  # silent reinterpretation of state IDs), or an image that keeps its
  # retired definition alive.
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

  # Catches: an image that borrows the host values its captured sends carry
  # instead of retaining them (ADR 0050). The actor is destroyed between
  # capture and reconstruction, so anything the image only borrowed is freed
  # under it: the frozen `{v: 1}` is the one host value in play, and it has
  # to survive its session.
  scenario 'an image retains the host values its captured sends carry' do
    freezer = spawn :freezer
    freezer.deliver 'arm'
    assert_equal 2, freezer.data('.v'), 'the send froze v=1 before the assignment'

    image = capture(freezer)
    binding = freezer.data
    core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: freezer.handle))
    rebind(freezer, reconstruct(image, binding: binding))
    destroy_image(image)

    advance_time 10_000
    assert_equal 1, freezer.data('.got'), 'the reconstructed send carried the value captured with it'
  end

  # Catches: an image deadline that is relative, stale, or missing, which a
  # host needs to wake an unloaded actor on time.
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

    # With several sends it is the earliest, not whichever the image happens
    # to hold first: the sends are scheduled longest-delay first on purpose.
    chart = TinyactorsTest::Chart.define(:three_deadlines) do
      state :ready do
        on_entry do
          send_event 'third', delay: '30s', id: :c
          send_event 'second', delay: '20s', id: :b
          send_event 'first', delay: '15s', id: :a
        end
      end
    end
    many = spawn chart
    image = capture(many)
    assert_equal 3, many.sends.size
    assert_equal 25_000, image_deadline(image), 'the earliest of several deadlines'
    destroy_image(image)
  end
end
