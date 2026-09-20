# frozen_string_literal: true

# Session images (ADR 0038, ADR 0039, ADR 0026, ADR 0051): capture records
# the whole interpreter session without changing it; restoring it against the
# exact definition revision and the host's data-model binding continues it as
# if nothing happened, except that invocations are lost and reported as
# error.communication.invoke.<id>.unavailable.
#
# Restoring is ta_actor_load(), which is a residency change of a *named*
# actor (ADR 0051): a session has to have a name to be restored under, so
# every scenario here that restores one uses a named actor and
# cycle_residency (capture, unload, load). Scenarios that only capture need
# no name.
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

  # Catches: an image of a paused actor that loses the current external
  # event or the internal queue, so the macrostep restarts, repeats a
  # microstep, or skips `counted`.
  #
  # The `ping` is the one thing a residency change does not carry. The relay
  # sends it to itself, which is a delivery to *this* session and so names a
  # runtime ID (see ta_send_destination); the router accepted it and is
  # holding it from before the capture. A delayed self-send survives, because
  # the image carries it and load rebinds it — but this one is already the
  # host's, and ADR 0014 makes fencing inbound events across a residency
  # change the host's job. This router does not re-fence, so the ping is
  # delivered to the session it named and lost. Core cannot fix it: it does
  # not know what the host is holding.
  expect_failure 'the router re-delivers a held ping to the session it named',
                 message: /expected \["go", "n=5", "counted", "ping"\], got \["go", "n=5", "counted"\]/
  scenario 'a macrostep captured while paused continues where it paused' do
    router = recording_router(mode: :hold)
    relay = spawn :relay, name: 'r1'
    relay.post('go')
    3.times { assert_equal 'paused', relay.resume(budget: 1) }
    before = relay.observe
    assert_equal 2, before.dig(:data, :n)

    cycle_residency(relay)
    assert_equal before, relay.observe, 'the paused session is restored exactly'
    assert_equal 'go', event_text(relay.inspect_actor.dig(:current_event, :name))
    assert_equal 'paused', relay.resume(budget: 1)
    assert_equal 3, relay.data('.n'), 'the next resume runs the next microstep'
    run_system('finish the macrostep')
    router.held.each { |delivery| router.release(delivery[:id]) }
    run_system('release the self-sent ping')
    assert_equal ['go', 'n=5', 'counted', 'ping'], relay.data('.log')
  end

  # Catches: restoration that restarts invocations (a duplicate external
  # effect), rebinds them silently, reports the loss in the wrong order or
  # with the wrong envelope, or still accepts events from a lost service.
  scenario 'restoration reports each lost invocation once, in invocation order' do
    service = scripted_invoker('urn:tinyactors:test:service')
    pair = spawn :service_pair, name: 'p1'
    starts = service.started
    assert_equal 2, starts.size

    cycle_residency(pair)
    run_system('restored')
    assert_equal 2, service.started.size, 'restoration starts no service'
    assert_equal [{ name: 'error.communication.invoke.first.unavailable', type: 'platform', invokeid: nil },
                  { name: 'error.communication.invoke.second.unavailable', type: 'platform', invokeid: nil }],
                 pair.data('.errors')
    assert_equal [], pair.observe[:invokes], 'no invocation is active after restoration'

    # The invocation is retired, so reporting for it names no invocation.
    lost = starts[0]
    report = ta.actor.report_invoke(system: system_handle, actor: pair.handle, invoke_id: lost[:invoke_id],
                                    kind: 'event', event: { name: event_name('reply'), type: 'external',
                                                            invoke_id: lost[:invoke_id] })
    assert_equal 'err_not_found', report[:result]
    run_system('late reply from the lost service')
    assert_equal [], pair.data('.replies'), 'events from a lost invocation are ignored'
  end

  # Catches: restoration against any definition of the same family (a silent
  # reinterpretation of state IDs), or an image that keeps its retired
  # definition alive.
  scenario 'restoration needs the exact definition revision' do
    light = spawn name: 'l1'
    advance_time 10_000
    image = capture(light)
    binding = light.data
    original = definition
    assert_equal 'ok', unload(light, image)
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
    assert_equal 'err_definition_mismatch', ta.actor.load(system: system_handle, image: image,
                                                          datamodel_binding: binding)[:result]

    register.call(spec)
    assert_equal 'ok', load(light, image, binding: binding)
    destroy_image(image)
    assert_equal [{ due: 30_000, event: 'timer' }], light.sends.map { |send| send.slice(:due, :event) }
    advance_time 20_000
    assert_equal %i[operating green], light.active, 'the restored timer fires at its original due time'
    assert_equal({ cycles: 0 }, light.data)
  end

  # Catches: an image that borrows the host values its captured sends carry
  # instead of retaining them (ADR 0050). The session that held them is gone
  # between capture and restoration, so anything the image only borrowed is
  # freed under it: the frozen `{v: 1}` is the one host value in play, and it
  # has to survive its session.
  scenario 'an image retains the host values its captured sends carry' do
    freezer = spawn :freezer, name: 'f1'
    freezer.deliver 'arm'
    assert_equal 2, freezer.data('.v'), 'the send froze v=1 before the assignment'

    cycle_residency(freezer)

    advance_time 10_000
    assert_equal 1, freezer.data('.got'), 'the restored send carried the value captured with it'
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
